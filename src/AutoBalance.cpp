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
#include "Message.h"

#if AC_COMPILER == AC_COMPILER_GNU
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

using namespace Acore::ChatCommands;

enum ScalingMethod {
    AUTOBALANCE_SCALING_FIXED,
    AUTOBALANCE_SCALING_DYNAMIC
};

enum BaseValueType {
    AUTOBALANCE_HEALTH,
    AUTOBALANCE_DAMAGE_HEALING
};

enum Relevance {
    AUTOBALANCE_RELEVANCE_FALSE,
    AUTOBALANCE_RELEVANCE_TRUE,
    AUTOBALANCE_RELEVANCE_UNCHECKED
};

enum Damage_Healing_Debug_Phase {
    AUTOBALANCE_DAMAGE_HEALING_DEBUG_PHASE_BEFORE,
    AUTOBALANCE_DAMAGE_HEALING_DEBUG_PHASE_AFTER
};

struct World_Multipliers {
    float scaled = 1.0f;
    float unscaled = 1.0f;
};

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

    uint64_t mapConfigTime = 1;                     // the last map config time that this creature was updated

    uint32 instancePlayerCount = 0;                 // the number of players this creature has been scaled for
    uint8 selectedLevel = 0;                        // the level that this creature should be set to

    float DamageMultiplier = 1.0f;                  // per-player damage multiplier (no level scaling)
    float ScaledDamageMultiplier = 1.0f;            // per-player and level scaling damage multiplier

    float HealthMultiplier = 1.0f;                  // per-player health multiplier (no level scaling)
    float ScaledHealthMultiplier = 1.0f;            // per-player and level scaling health multiplier

    float ManaMultiplier = 1.0f;                    // per-player mana multiplier (no level scaling)
    float ScaledManaMultiplier = 1.0f;              // per-player and level scaling mana multiplier

    float ArmorMultiplier = 1.0f;                   // per-player armor multiplier (no level scaling)
    float ScaledArmorMultiplier = 1.0f;             // per-player and level scaling armor multiplier

    float CCDurationMultiplier = 1.0f;              // per-player crowd control duration multiplier (level scaling doesn't affect this)

    float XPModifier = 1.0f;                        // per-player XP modifier (level scaling provided by normal XP distribution)
    float MoneyModifier = 1.0f;                     // per-player money modifier (no level scaling)

    uint8 UnmodifiedLevel = 0;                      // original level of the creature as determined by the game

    bool isActive = false;                          // whether or not the current creature is affecting map stats. May change as conditions change.
    bool wasAliveNowDead = false;                   // whether or not the creature was alive and is now dead
    bool isInCreatureList = false;                  // whether or not the creature is in the map's creature list
    bool isBrandNew = false;                        // whether or not the creature is brand new to the map (hasn't been added to the world yet)
    bool neverLevelScale = false;                   // whether or not the creature should never be level scaled (can still be player scaled)

    uint32 initialMaxHealth = 0;                    // stored max health value to be applied just before being added to the world

    // creature->IsSummon()                         // whether or not the creature is a summon
    Creature* summoner = nullptr;                   // the creature that summoned this creature
    std::string summonerName = "";                  // the name of the creature that summoned this creature
    uint8 summonerLevel = 0;                        // the level of the creature that summoned this creature
    bool isCloneOfSummoner = false;                 // whether or not the creature is a clone of its summoner

    Relevance relevance = AUTOBALANCE_RELEVANCE_UNCHECKED;  // whether or not the creature is relevant for scaling
};

class AutoBalanceMapInfo : public DataMap::Base
{
public:
    AutoBalanceMapInfo() {}

    uint64_t globalConfigTime = 1;                   // the last global config time that this map was updated
    uint64_t mapConfigTime = 1;                      // the last map config time that this map was updated

    uint8 playerCount = 0;                           // the actual number of non-GM players in the map
    uint8 adjustedPlayerCount = 0;                   // the currently difficulty level expressed as number of players
    uint8 minPlayers = 1;                            // will be set by the config

    uint8 mapLevel = 0;                              // calculated from the avgCreatureLevel
    uint8 lowestPlayerLevel = 0;                     // the lowest-level player in the map
    uint8 highestPlayerLevel = 0;                    // the highest-level player in the map

    uint8 lfgMinLevel = 0;                           // the minimum level for the map according to LFG
    uint8 lfgTargetLevel = 0;                        // the target level for the map according to LFG
    uint8 lfgMaxLevel = 0;                           // the maximum level for the map according to LFG

    uint8 worldMultiplierTargetLevel = 0;            // the level of the pseudo-creature that the world modifiers scale to
    float worldDamageHealingMultiplier = 1.0f;       // the damage/healing multiplier for the world (where source isn't an enemy creature)
    float scaledWorldDamageHealingMultiplier = 1.0f; // the damage/healing multiplier for the world (where source isn't an enemy creature)
    float worldHealthMultiplier = 1.0f;              // the "health" multiplier for any destructible buildings in the map

    bool enabled = false;                            // should AutoBalance make any changes to this map or its creatures?

    std::vector<Creature*> allMapCreatures;          // all creatures in the map, active and non-active
    std::vector<Player*> allMapPlayers;              // all players that are currently in the map

    bool combatLocked = false;                       // whether or not the map is combat locked
    bool combatLockTripped = false;                  // set to true when combat locking was needed during this current combat (some tried to leave)
    uint8 combatLockMinPlayers = 0;                  // the instance cannot be set to less than this number of players until combat ends

    uint8 highestCreatureLevel = 0;                  // the highest-level creature in the map
    uint8 lowestCreatureLevel = 0;                   // the lowest-level creature in the map
    float avgCreatureLevel = 0;                      // the average level of all active creatures in the map (continuously updated)
    uint32 activeCreatureCount = 0;                  // the number of creatures in the map that are included in the map's stats (not necessarily alive)

    bool isLevelScalingEnabled = false;              // whether level scaling is enabled on this map
    uint8 levelScalingSkipHigherLevels;              // used to determine if this map should scale or not
    uint8 levelScalingSkipLowerLevels;               // used to determine if this map should scale or not
    uint8 levelScalingDynamicCeiling;                // how many levels MORE than the highestPlayerLevel creature should be scaled to
    uint8 levelScalingDynamicFloor;                  // how many levels LESS than the highestPlayerLevel creature should be scaled to

    uint8 prevMapLevel = 0;                          // used to reduce calculations when they are not necessary
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

uint64_t GetCurrentConfigTime()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// spell IDs that spend player health
// player abilities don't actually appear to be caught by `ModifySpellDamageTaken`,
// but I'm leaving them here in case they ever DO get caught by it
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
    29858,      // Soulshatter
    55213       // Unholy Frenzy
};

// creature IDs that should never be considered clones
// handles cases where a creature is spawned by another creature, but is not a clone (doesn't retain health/mana values)
static std::list<uint32> creatureIDsThatAreNotClones =
{
    16152       // Attumen the Huntsman (Karazhan) combined form
};

// spell IDs that should never be modified
// handles cases where a spell is reflecting damage or otherwise converting player damage to something else
static std::list<uint32> spellIdsToNeverModify =
{
    1177        // Twin Empathy (AQ40 Twin Emperors, only in `spell_dbc` database table)
};

// spacer used for logging
std::string SPACER = "------------------------------------------------";

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

// Track the initial config time
static uint64_t globalConfigTime = GetCurrentConfigTime();

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
    if (map->IsDungeon())
    {
        // if globally disabled, return false
        if (!EnableGlobal)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: {} ({}{}) - Not enabled because EnableGlobal is false",
                        map->GetMapName(),
                        map->GetId(),
                        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
            );
            return false;
        }

        InstanceMap* instanceMap = map->ToInstanceMap();

        // if there wasn't one, then we're not in an instance
        if (!instanceMap)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: {} ({}{}) - Not enabled for the base map without an Instance ID.",
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
            LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: {} ({}{}, {}-player {}) - Not enabled because the map ID is disabled via configuration.",
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
            //LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: Heroic Enables - 5:{} 10:{} 25:{} Other:{}",
            //            Enable5MHeroic, Enable10MHeroic, Enable25MHeroic, EnableOtherHeroic);

            if (instanceMap->GetMaxPlayers() <= 5)
            {
                sizeDifficultyEnabled = Enable5MHeroic;
            }
            else if (instanceMap->GetMaxPlayers() <= 10)
            {
                sizeDifficultyEnabled = Enable10MHeroic;
            }
            else if (instanceMap->GetMaxPlayers() <= 25)
            {
                sizeDifficultyEnabled = Enable25MHeroic;
            }
            else
            {
                sizeDifficultyEnabled = EnableOtherHeroic;
            }
        }
        else
        {
            //LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: Normal Enables - 5:{} 10:{} 15:{} 20:{} 25:{} 40:{} Other:{}",
            //            Enable5M, Enable10M, Enable15M, Enable20M, Enable25M, Enable40M, EnableOtherNormal);
            if (instanceMap->GetMaxPlayers() <= 5)
            {
                sizeDifficultyEnabled = Enable5M;
            }
            else if (instanceMap->GetMaxPlayers() <= 10)
            {
                sizeDifficultyEnabled = Enable10M;
            }
            else if (instanceMap->GetMaxPlayers() <= 15)
            {
                sizeDifficultyEnabled = Enable15M;
            }
            else if (instanceMap->GetMaxPlayers() <= 20)
            {
                sizeDifficultyEnabled = Enable20M;
            }
            else if (instanceMap->GetMaxPlayers() <= 25)
            {
                sizeDifficultyEnabled = Enable25M;
            }
            else if (instanceMap->GetMaxPlayers() <= 40)
            {
                sizeDifficultyEnabled = Enable40M;
            }
            else
            {
                sizeDifficultyEnabled = EnableOtherNormal;
            }
        }

        if (sizeDifficultyEnabled)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: Map {} ({}{}, {}-player {}) | Enabled for AutoBalancing.",
                      map->GetMapName(),
                      map->GetId(),
                      map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                      instanceMap->GetMaxPlayers(),
                      instanceMap->IsHeroic() ? "Heroic" : "Normal"
            );
        }
        else
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: Map {} ({}{}, {}-player {}) | Not enabled because its size and difficulty are disabled via configuration.",
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
        LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: Map {} ({}{}) | Not enabled because the map is not an instance.",
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

bool isBossOrBossSummon(Creature* creature, bool log = false)
{
    // no creature? not a boss
    if (!creature)
    {
        LOG_INFO("module.AutoBalance", "AutoBalance::isBossOrBossSummon: Creature is null.");
        return false;
    }

    // if this creature is a boss, return true
    if (creature->IsDungeonBoss() || creature->isWorldBoss())
    {
        if (log)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::isBossOrBossSummon: {} ({}{}) is a boss.",
                        creature->GetName(),
                        creature->GetEntry(),
                        creature->GetInstanceId() ? "-" + std::to_string(creature->GetInstanceId()) : ""
            );
        }

        return true;
    }


    // if this creature is a summon of a boss, return true
    if (
        creature->IsSummon() &&
        creature->ToTempSummon() &&
        creature->ToTempSummon()->GetSummoner() &&
        creature->ToTempSummon()->GetSummoner()->ToCreature()
        )
    {
        Creature* summoner = creature->ToTempSummon()->GetSummoner()->ToCreature();

        if (summoner)
        {
            if (summoner->IsDungeonBoss() || summoner->isWorldBoss())
            {
                if (log)
                {
                    LOG_DEBUG("module.AutoBalance", "AutoBalance::isBossOrBossSummon: {} ({}) is a summon of boss {}({}).",
                                creature->GetName(),
                                creature->GetEntry(),
                                summoner->GetName(),
                                summoner->GetEntry()
                    );
                }

                return true;
            }
            else
            {
                if (log)
                {
                    LOG_DEBUG("module.AutoBalance", "AutoBalance::isBossOrBossSummon: {} ({}) is a summon of {}({}).",
                                creature->GetName(),
                                creature->GetEntry(),
                                summoner->GetName(),
                                summoner->GetEntry()
                    );
                }
                return false;
            }
        }
    }

    // not a boss
    if (log)
    {
        // LOG_DEBUG("module.AutoBalance", "AutoBalance::isBossOrBossSummon: {} ({}) is NOT a boss.",
        //             creature->GetName(),
        //             creature->GetEntry()
        // );
    }

    return false;
}

bool isCreatureRelevant(Creature* creature) {
    // if the creature is gone, return false
    if (!creature)
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::isCreatureRelevant: Creature is null.");
        return false;
    }

    // if this creature isn't assigned to a map, make no changes
    if (!creature->GetMap() || !creature->GetMap()->IsDungeon())
    {
        // executed every Creature update for every world creature, enable carefully
        // LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::isCreatureRelevant: Creature {} ({}) isn't in a dungeon.",
        //             creature->GetName(),
        //             creature->GetLevel()
        // );
        return false;
    }

    // get the creature's info
    AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

    // if this creature has been already been evaluated, just return the previous evaluation
    if (creatureABInfo->relevance == AUTOBALANCE_RELEVANCE_FALSE)
    {
        return false;
    }
    else if (creatureABInfo->relevance == AUTOBALANCE_RELEVANCE_TRUE)
    {
        return true;
    }
    // otherwise the value is AUTOBALANCE_RELEVANCE_UNCHECKED, so it needs checking

    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::isCreatureRelevant: Creature {} ({}) | Needs to be evaluated.",
                creature->GetName(),
                creatureABInfo->UnmodifiedLevel
    );

    // get the creature's map's info
    Map* creatureMap = creature->GetMap();
    AutoBalanceMapInfo *mapABInfo=creatureMap->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
    InstanceMap* instanceMap = creatureMap->ToInstanceMap();

    // if this creature is in the dungeon's base map, make no changes
    if (!(instanceMap))
    {
        creatureABInfo->relevance = AUTOBALANCE_RELEVANCE_FALSE;
        LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::isCreatureRelevant: Creature {} ({}) | is in the base map, no changes. Marked for skip.",
                    creature->GetName(),
                    creatureABInfo->UnmodifiedLevel
        );
        return false;
    }

    // if this is a pet or summon controlled by the player, make no changes
    if ((creature->IsHunterPet() || creature->IsPet() || creature->IsSummon()) && creature->IsControlledByPlayer())
    {
        creatureABInfo->relevance = AUTOBALANCE_RELEVANCE_FALSE;
        LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::isCreatureRelevant: Creature {} ({}) | is a pet or summon controlled by the player, no changes. Marked for skip.",
                    creature->GetName(),
                    creatureABInfo->UnmodifiedLevel
        );

        return false;
    }

    // if this is a player temporary summon (that isn't actively trying to kill the players), make no changes
    if (
        creature->ToTempSummon() &&
        creature->ToTempSummon()->GetSummoner() &&
        creature->ToTempSummon()->GetSummoner()->ToPlayer()
    )
    {
        // if this creature is hostile to any non-charmed player, it should be scaled
        bool isHostileToAnyValidPlayer = false;
        TempSummon* creatureTempSummon = creature->ToTempSummon();
        Player* summonerPlayer = creatureTempSummon->GetSummoner()->ToPlayer();

        for (std::vector<Player*>::const_iterator playerIterator = mapABInfo->allMapPlayers.begin(); playerIterator != mapABInfo->allMapPlayers.end(); ++playerIterator)
        {
            Player* thisPlayer = *playerIterator;

            // is this a valid player?
            if (!thisPlayer->IsGameMaster() &&
                !thisPlayer->IsCharmed() &&
                !thisPlayer->IsHostileToPlayers() &&
                !thisPlayer->IsHostileTo(summonerPlayer) &&
                thisPlayer->IsAlive()
            )
            {
                // if this is a guardian and the owner is not hostile to this player, skip
                if
                (
                    creatureTempSummon->IsGuardian() &&
                    !thisPlayer->IsHostileTo(summonerPlayer)
                )
                {
                    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::isCreatureRelevant: Creature {} ({}) | is a guardian of player {}, who is not hostile to valid player {}.",
                                creature->GetName(),
                                creatureABInfo->UnmodifiedLevel,
                                summonerPlayer->GetName(),
                                thisPlayer->GetName()
                    );

                    continue;
                }
                // special case for totems?
                else if
                (
                    creature->IsTotem() &&
                    !thisPlayer->IsHostileTo(summonerPlayer)
                )
                {
                    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::isCreatureRelevant: Creature {} ({}) | is a totem of player {}, who is not hostile to valid player {}.",
                                creature->GetName(),
                                creatureABInfo->UnmodifiedLevel,
                                summonerPlayer->GetName(),
                                thisPlayer->GetName()
                    );

                    continue;
                }

                // if the creature is hostile to this valid player,
                // unfortunately, `creature->IsHostileTo(thisPlayer)` returns true for cases when it is not actually hostile
                else if (
                    thisPlayer->isTargetableForAttack(true, creature)
                )
                {
                    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::isCreatureRelevant: Creature {} ({}) | is a player temporary summon hostile to valid player {}.",
                                creature->GetName(),
                                creatureABInfo->UnmodifiedLevel,
                                thisPlayer->GetName()
                    );

                    isHostileToAnyValidPlayer = true;
                    break;
                }
            }
        }

        if (!isHostileToAnyValidPlayer)
        {
            // since no players are hostile to this creature, it should not be scaled
            creatureABInfo->relevance = AUTOBALANCE_RELEVANCE_FALSE;
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::isCreatureRelevant: Creature {} ({}) | is player-summoned and non-hostile, no changes. Marked for skip.",
                creature->GetName(),
                creatureABInfo->UnmodifiedLevel
            );

            return false;
        }

    }

    // if this is a flavor critter
    // level and health checks for some nasty level 1 critters in some encounters
    if ((creature->IsCritter() && creatureABInfo->UnmodifiedLevel <= 5 && creature->GetMaxHealth() < 100))
    {
        creatureABInfo->relevance = AUTOBALANCE_RELEVANCE_FALSE;
        LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::isCreatureRelevant: Creature {} ({}) | is a non-relevant critter, no changes. Marked for skip.",
                    creature->GetName(),
                    creatureABInfo->UnmodifiedLevel
        );

        return false;
    }

    // survived to here, creature is relevant
    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::isCreatureRelevant: Creature {} ({}) | is relevant. Marked for processing.",
                creature->GetName(),
                creatureABInfo->UnmodifiedLevel
    );
    creatureABInfo->relevance = AUTOBALANCE_RELEVANCE_TRUE;
    return true;

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
        if (maxNumberOfPlayers <= 5)
        {
            inflectionValue *= InflectionPointHeroic;
            curveFloor = InflectionPointHeroicCurveFloor;
            curveCeiling = InflectionPointHeroicCurveCeiling;
        }
        else if (maxNumberOfPlayers <= 10)
        {
            inflectionValue *= InflectionPointRaid10MHeroic;
            curveFloor = InflectionPointRaid10MHeroicCurveFloor;
            curveCeiling = InflectionPointRaid10MHeroicCurveCeiling;
        }
        else if (maxNumberOfPlayers <= 25)
        {
            inflectionValue *= InflectionPointRaid25MHeroic;
            curveFloor = InflectionPointRaid25MHeroicCurveFloor;
            curveCeiling = InflectionPointRaid25MHeroicCurveCeiling;
        }
        else
        {
            inflectionValue *= InflectionPointRaidHeroic;
            curveFloor = InflectionPointRaidHeroicCurveFloor;
            curveCeiling = InflectionPointRaidHeroicCurveCeiling;
        }
    }
    else
    {
        if (maxNumberOfPlayers <= 5)
        {
            inflectionValue *= InflectionPoint;
            curveFloor = InflectionPointCurveFloor;
            curveCeiling = InflectionPointCurveCeiling;
        }
        else if (maxNumberOfPlayers <= 10)
        {
            inflectionValue *= InflectionPointRaid10M;
            curveFloor = InflectionPointRaid10MCurveFloor;
            curveCeiling = InflectionPointRaid10MCurveCeiling;
        }
        else if (maxNumberOfPlayers <= 15)
        {
            inflectionValue *= InflectionPointRaid15M;
            curveFloor = InflectionPointRaid15MCurveFloor;
            curveCeiling = InflectionPointRaid15MCurveCeiling;
        }
        else if (maxNumberOfPlayers <= 20)
        {
            inflectionValue *= InflectionPointRaid20M;
            curveFloor = InflectionPointRaid20MCurveFloor;
            curveCeiling = InflectionPointRaid20MCurveCeiling;
        }
        else if (maxNumberOfPlayers <= 25)
        {
            inflectionValue *= InflectionPointRaid25M;
            curveFloor = InflectionPointRaid25MCurveFloor;
            curveCeiling = InflectionPointRaid25MCurveCeiling;
        }
        else if (maxNumberOfPlayers <= 40)
        {
            inflectionValue *= InflectionPointRaid40M;
            curveFloor = InflectionPointRaid40MCurveFloor;
            curveCeiling = InflectionPointRaid40MCurveCeiling;
        }
        else
        {
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

        if (instanceMap->IsHeroic())
        {
            if (maxNumberOfPlayers <= 5)
            {
                bossInflectionPointMultiplier = InflectionPointHeroicBoss;
            }
            else if (maxNumberOfPlayers <= 10)
            {
                bossInflectionPointMultiplier = InflectionPointRaid10MHeroicBoss;
            }
            else if (maxNumberOfPlayers <= 25)
            {
                bossInflectionPointMultiplier = InflectionPointRaid25MHeroicBoss;
            }
            else
            {
                bossInflectionPointMultiplier = InflectionPointRaidHeroicBoss;
            }
        }
        else
        {
            if (maxNumberOfPlayers <= 5)
            {
                bossInflectionPointMultiplier = InflectionPointBoss;
            }
            else if (maxNumberOfPlayers <= 10)
            {
                bossInflectionPointMultiplier = InflectionPointRaid10MBoss;
            }
            else if (maxNumberOfPlayers <= 15)
            {
                bossInflectionPointMultiplier = InflectionPointRaid15MBoss;
            }
            else if (maxNumberOfPlayers <= 20)
            {
                bossInflectionPointMultiplier = InflectionPointRaid20MBoss;
            }
            else if (maxNumberOfPlayers <= 25)
            {
                bossInflectionPointMultiplier = InflectionPointRaid25MBoss;
            }
            else if (maxNumberOfPlayers <= 40)
            {
                bossInflectionPointMultiplier = InflectionPointRaid40MBoss;
            }
            else
            {
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

void getStatModifiersDebug(Map *map, Creature *creature, std::string message)
{
    // if we have a creature, include that in the output
    if (creature)
    {
        // get the creature's info
        AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance::getStatModifiers: Map {} ({}{}) | Creature {} ({}{}) | {}",
                    map->GetMapName(),
                    map->GetId(),
                    map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                    creature->GetName(),
                    creatureABInfo->UnmodifiedLevel,
                    creatureABInfo->selectedLevel ? "->" + std::to_string(creatureABInfo->selectedLevel) : "",
                    message
        );
    }
    // if no creature was provided, remove that from the output
    else
    {
        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance::getStatModifiers: Map {} ({}{}) | {}",
                    map->GetMapName(),
                    map->GetId(),
                    map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                    message
        );
    }
}

AutoBalanceStatModifiers getStatModifiers (Map* map, Creature* creature = nullptr)
{
    // get the instance's InstanceMap
    InstanceMap* instanceMap = map->ToInstanceMap();

    // map variables
    uint32 maxNumberOfPlayers = instanceMap->GetMaxPlayers();
    uint32 mapId = map->GetId();

    // get the creature's info if a creature was specified
    AutoBalanceCreatureInfo* creatureABInfo = nullptr;
    if (creature)
    {
        creatureABInfo = creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");
    }

    // this will be the return value
    AutoBalanceStatModifiers statModifiers;

    // Apply the per-instance-type modifiers first
    // AutoBalance.StatModifier*(.Boss).<stat>
    if (instanceMap->IsHeroic()) // heroic
    {
        if (maxNumberOfPlayers <= 5)
        {
            if (creature && isBossOrBossSummon(creature))
            {
                statModifiers.global = StatModifierHeroic_Boss_Global;
                statModifiers.health = StatModifierHeroic_Boss_Health;
                statModifiers.mana = StatModifierHeroic_Boss_Mana;
                statModifiers.armor = StatModifierHeroic_Boss_Armor;
                statModifiers.damage = StatModifierHeroic_Boss_Damage;
                statModifiers.ccduration = StatModifierHeroic_Boss_CCDuration;

                getStatModifiersDebug(map, creature, "1 to 5 Player Heroic Boss");
            }
            else
            {
                statModifiers.global = StatModifierHeroic_Global;
                statModifiers.health = StatModifierHeroic_Health;
                statModifiers.mana = StatModifierHeroic_Mana;
                statModifiers.armor = StatModifierHeroic_Armor;
                statModifiers.damage = StatModifierHeroic_Damage;
                statModifiers.ccduration = StatModifierHeroic_CCDuration;

                getStatModifiersDebug(map, creature, "1 to 5 Player Heroic");
            }
        }
        else if (maxNumberOfPlayers <= 10)
        {
            if (creature && isBossOrBossSummon(creature))
            {
                statModifiers.global = StatModifierRaid10MHeroic_Boss_Global;
                statModifiers.health = StatModifierRaid10MHeroic_Boss_Health;
                statModifiers.mana = StatModifierRaid10MHeroic_Boss_Mana;
                statModifiers.armor = StatModifierRaid10MHeroic_Boss_Armor;
                statModifiers.damage = StatModifierRaid10MHeroic_Boss_Damage;
                statModifiers.ccduration = StatModifierRaid10MHeroic_Boss_CCDuration;

                getStatModifiersDebug(map, creature, "10 Player Heroic Boss");
            }
            else
            {
                statModifiers.global = StatModifierRaid10MHeroic_Global;
                statModifiers.health = StatModifierRaid10MHeroic_Health;
                statModifiers.mana = StatModifierRaid10MHeroic_Mana;
                statModifiers.armor = StatModifierRaid10MHeroic_Armor;
                statModifiers.damage = StatModifierRaid10MHeroic_Damage;
                statModifiers.ccduration = StatModifierRaid10MHeroic_CCDuration;

                getStatModifiersDebug(map, creature, "10 Player Heroic");
            }
        }
        else if (maxNumberOfPlayers <= 25)
        {
            if (creature && isBossOrBossSummon(creature))
            {
                statModifiers.global = StatModifierRaid25MHeroic_Boss_Global;
                statModifiers.health = StatModifierRaid25MHeroic_Boss_Health;
                statModifiers.mana = StatModifierRaid25MHeroic_Boss_Mana;
                statModifiers.armor = StatModifierRaid25MHeroic_Boss_Armor;
                statModifiers.damage = StatModifierRaid25MHeroic_Boss_Damage;
                statModifiers.ccduration = StatModifierRaid25MHeroic_Boss_CCDuration;

                getStatModifiersDebug(map, creature, "25 Player Heroic Boss");
            }
            else
            {
                statModifiers.global = StatModifierRaid25MHeroic_Global;
                statModifiers.health = StatModifierRaid25MHeroic_Health;
                statModifiers.mana = StatModifierRaid25MHeroic_Mana;
                statModifiers.armor = StatModifierRaid25MHeroic_Armor;
                statModifiers.damage = StatModifierRaid25MHeroic_Damage;
                statModifiers.ccduration = StatModifierRaid25MHeroic_CCDuration;

                getStatModifiersDebug(map, creature, "25 Player Heroic");
            }
        }
        else
        {
            if (creature && isBossOrBossSummon(creature))
            {
                statModifiers.global = StatModifierRaidHeroic_Boss_Global;
                statModifiers.health = StatModifierRaidHeroic_Boss_Health;
                statModifiers.mana = StatModifierRaidHeroic_Boss_Mana;
                statModifiers.armor = StatModifierRaidHeroic_Boss_Armor;
                statModifiers.damage = StatModifierRaidHeroic_Boss_Damage;
                statModifiers.ccduration = StatModifierRaidHeroic_Boss_CCDuration;

                getStatModifiersDebug(map, creature, "?? Player Heroic Boss");
            }
            else
            {
                statModifiers.global = StatModifierRaidHeroic_Global;
                statModifiers.health = StatModifierRaidHeroic_Health;
                statModifiers.mana = StatModifierRaidHeroic_Mana;
                statModifiers.armor = StatModifierRaidHeroic_Armor;
                statModifiers.damage = StatModifierRaidHeroic_Damage;
                statModifiers.ccduration = StatModifierRaidHeroic_CCDuration;

                getStatModifiersDebug(map, creature, "?? Player Heroic");
            }
        }
    }
    else // non-heroic
    {
        if (maxNumberOfPlayers <= 5)
        {
            if (creature && isBossOrBossSummon(creature))
            {
                statModifiers.global = StatModifier_Boss_Global;
                statModifiers.health = StatModifier_Boss_Health;
                statModifiers.mana = StatModifier_Boss_Mana;
                statModifiers.armor = StatModifier_Boss_Armor;
                statModifiers.damage = StatModifier_Boss_Damage;
                statModifiers.ccduration = StatModifier_Boss_CCDuration;

                getStatModifiersDebug(map, creature, "1 to 5 Player Normal Boss");
            }
            else
            {
                statModifiers.global = StatModifier_Global;
                statModifiers.health = StatModifier_Health;
                statModifiers.mana = StatModifier_Mana;
                statModifiers.armor = StatModifier_Armor;
                statModifiers.damage = StatModifier_Damage;
                statModifiers.ccduration = StatModifier_CCDuration;

                getStatModifiersDebug(map, creature, "1 to 5 Player Normal");
            }
        }
        else if (maxNumberOfPlayers <= 10)
        {
            if (creature && isBossOrBossSummon(creature))
            {
                statModifiers.global = StatModifierRaid10M_Boss_Global;
                statModifiers.health = StatModifierRaid10M_Boss_Health;
                statModifiers.mana = StatModifierRaid10M_Boss_Mana;
                statModifiers.armor = StatModifierRaid10M_Boss_Armor;
                statModifiers.damage = StatModifierRaid10M_Boss_Damage;
                statModifiers.ccduration = StatModifierRaid10M_Boss_CCDuration;

                getStatModifiersDebug(map, creature, "10 Player Normal Boss");
            }
            else
            {
                statModifiers.global = StatModifierRaid10M_Global;
                statModifiers.health = StatModifierRaid10M_Health;
                statModifiers.mana = StatModifierRaid10M_Mana;
                statModifiers.armor = StatModifierRaid10M_Armor;
                statModifiers.damage = StatModifierRaid10M_Damage;
                statModifiers.ccduration = StatModifierRaid10M_CCDuration;

                getStatModifiersDebug(map, creature, "10 Player Normal");
            }
        }
        else if (maxNumberOfPlayers <= 15)
        {
            if (creature && isBossOrBossSummon(creature))
            {
                statModifiers.global = StatModifierRaid15M_Boss_Global;
                statModifiers.health = StatModifierRaid15M_Boss_Health;
                statModifiers.mana = StatModifierRaid15M_Boss_Mana;
                statModifiers.armor = StatModifierRaid15M_Boss_Armor;
                statModifiers.damage = StatModifierRaid15M_Boss_Damage;
                statModifiers.ccduration = StatModifierRaid15M_Boss_CCDuration;

                getStatModifiersDebug(map, creature, "15 Player Normal Boss");
            }
            else
            {
                statModifiers.global = StatModifierRaid15M_Global;
                statModifiers.health = StatModifierRaid15M_Health;
                statModifiers.mana = StatModifierRaid15M_Mana;
                statModifiers.armor = StatModifierRaid15M_Armor;
                statModifiers.damage = StatModifierRaid15M_Damage;
                statModifiers.ccduration = StatModifierRaid15M_CCDuration;

                getStatModifiersDebug(map, creature, "15 Player Normal");
            }
        }
        else if (maxNumberOfPlayers <= 20)
        {
            if (creature && isBossOrBossSummon(creature))
            {
                statModifiers.global = StatModifierRaid20M_Boss_Global;
                statModifiers.health = StatModifierRaid20M_Boss_Health;
                statModifiers.mana = StatModifierRaid20M_Boss_Mana;
                statModifiers.armor = StatModifierRaid20M_Boss_Armor;
                statModifiers.damage = StatModifierRaid20M_Boss_Damage;
                statModifiers.ccduration = StatModifierRaid20M_Boss_CCDuration;

                getStatModifiersDebug(map, creature, "20 Player Normal Boss");
            }
            else
            {
                statModifiers.global = StatModifierRaid20M_Global;
                statModifiers.health = StatModifierRaid20M_Health;
                statModifiers.mana = StatModifierRaid20M_Mana;
                statModifiers.armor = StatModifierRaid20M_Armor;
                statModifiers.damage = StatModifierRaid20M_Damage;
                statModifiers.ccduration = StatModifierRaid20M_CCDuration;

                getStatModifiersDebug(map, creature, "20 Player Normal");
            }
        }
        else if (maxNumberOfPlayers <= 25)
        {
            if (creature && isBossOrBossSummon(creature))
            {
                statModifiers.global = StatModifierRaid25M_Boss_Global;
                statModifiers.health = StatModifierRaid25M_Boss_Health;
                statModifiers.mana = StatModifierRaid25M_Boss_Mana;
                statModifiers.armor = StatModifierRaid25M_Boss_Armor;
                statModifiers.damage = StatModifierRaid25M_Boss_Damage;
                statModifiers.ccduration = StatModifierRaid25M_Boss_CCDuration;

                getStatModifiersDebug(map, creature, "25 Player Normal Boss");
            }
            else
            {
                statModifiers.global = StatModifierRaid25M_Global;
                statModifiers.health = StatModifierRaid25M_Health;
                statModifiers.mana = StatModifierRaid25M_Mana;
                statModifiers.armor = StatModifierRaid25M_Armor;
                statModifiers.damage = StatModifierRaid25M_Damage;
                statModifiers.ccduration = StatModifierRaid25M_CCDuration;

                getStatModifiersDebug(map, creature, "25 Player Normal");
            }
        }
        else if (maxNumberOfPlayers <= 40)
        {
            if (creature && isBossOrBossSummon(creature))
            {
                statModifiers.global = StatModifierRaid40M_Boss_Global;
                statModifiers.health = StatModifierRaid40M_Boss_Health;
                statModifiers.mana = StatModifierRaid40M_Boss_Mana;
                statModifiers.armor = StatModifierRaid40M_Boss_Armor;
                statModifiers.damage = StatModifierRaid40M_Boss_Damage;
                statModifiers.ccduration = StatModifierRaid40M_Boss_CCDuration;

                getStatModifiersDebug(map, creature, "40 Player Normal Boss");
            }
            else
            {
                statModifiers.global = StatModifierRaid40M_Global;
                statModifiers.health = StatModifierRaid40M_Health;
                statModifiers.mana = StatModifierRaid40M_Mana;
                statModifiers.armor = StatModifierRaid40M_Armor;
                statModifiers.damage = StatModifierRaid40M_Damage;
                statModifiers.ccduration = StatModifierRaid40M_CCDuration;

                getStatModifiersDebug(map, creature, "40 Player Normal");
            }
        }
        else
        {
            if (creature && isBossOrBossSummon(creature))
            {
                statModifiers.global = StatModifierRaid_Boss_Global;
                statModifiers.health = StatModifierRaid_Boss_Health;
                statModifiers.mana = StatModifierRaid_Boss_Mana;
                statModifiers.armor = StatModifierRaid_Boss_Armor;
                statModifiers.damage = StatModifierRaid_Boss_Damage;
                statModifiers.ccduration = StatModifierRaid_Boss_CCDuration;

                getStatModifiersDebug(map, creature, "?? Player Normal Boss");
            }
            else
            {
                statModifiers.global = StatModifierRaid_Global;
                statModifiers.health = StatModifierRaid_Health;
                statModifiers.mana = StatModifierRaid_Mana;
                statModifiers.armor = StatModifierRaid_Armor;
                statModifiers.damage = StatModifierRaid_Damage;
                statModifiers.ccduration = StatModifierRaid_CCDuration;

                getStatModifiersDebug(map, creature, "?? Player Normal");
            }
        }
    }

    // Per-Map Overrides
    // AutoBalance.StatModifier.Boss.PerInstance
    if (creature && isBossOrBossSummon(creature) && hasStatModifierBossOverride(mapId))
    {
        AutoBalanceStatModifiers* myStatModifierBossOverrides = &statModifierBossOverrides[mapId];

        if (myStatModifierBossOverrides->global != -1)      { statModifiers.global =      myStatModifierBossOverrides->global;      }
        if (myStatModifierBossOverrides->health != -1)      { statModifiers.health =      myStatModifierBossOverrides->health;      }
        if (myStatModifierBossOverrides->mana != -1)        { statModifiers.mana =        myStatModifierBossOverrides->mana;        }
        if (myStatModifierBossOverrides->armor != -1)       { statModifiers.armor =       myStatModifierBossOverrides->armor;       }
        if (myStatModifierBossOverrides->damage != -1)      { statModifiers.damage =      myStatModifierBossOverrides->damage;      }
        if (myStatModifierBossOverrides->ccduration != -1)  { statModifiers.ccduration =  myStatModifierBossOverrides->ccduration;  }

        getStatModifiersDebug(map, creature, "Boss Per-Instance Override");
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

        getStatModifiersDebug(map, creature, "Per-Instance Override");
    }

    // Per-creature modifiers applied last
    // AutoBalance.StatModifier.PerCreature
    if (creature && hasStatModifierCreatureOverride(creature->GetEntry()))
    {
        AutoBalanceStatModifiers* myCreatureOverrides = &statModifierCreatureOverrides[creature->GetEntry()];

        if (myCreatureOverrides->global != -1)      { statModifiers.global =      myCreatureOverrides->global;      }
        if (myCreatureOverrides->health != -1)      { statModifiers.health =      myCreatureOverrides->health;      }
        if (myCreatureOverrides->mana != -1)        { statModifiers.mana =        myCreatureOverrides->mana;        }
        if (myCreatureOverrides->armor != -1)       { statModifiers.armor =       myCreatureOverrides->armor;       }
        if (myCreatureOverrides->damage != -1)      { statModifiers.damage =      myCreatureOverrides->damage;      }
        if (myCreatureOverrides->ccduration != -1)  { statModifiers.ccduration =  myCreatureOverrides->ccduration;  }

        getStatModifiersDebug(map, creature, "Per-Creature Override");
    }

    if (creature)
    {
        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance::getStatModifiers: Map {} ({}{}) | Creature {} ({}{}) | Stat Modifiers = global: {} | health: {} | mana: {} | armor: {} | damage: {} | ccduration: {}",
                    map->GetMapName(),
                    map->GetId(),
                    map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                    creature->GetName(),
                    creatureABInfo->UnmodifiedLevel,
                    creatureABInfo->selectedLevel ? "->" + std::to_string(creatureABInfo->selectedLevel) : "",
                    statModifiers.global,
                    statModifiers.health,
                    statModifiers.mana,
                    statModifiers.armor,
                    statModifiers.damage,
                    statModifiers.ccduration == -1 ? 1.0f : statModifiers.ccduration
        );
    }
    else
    {
        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance::getStatModifiers: Map {} ({}{}) | Stat Modifiers = global: {} | health: {} | mana: {} | armor: {} | damage: {} | ccduration: {}",
                    map->GetMapName(),
                    map->GetId(),
                    map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                    statModifiers.global,
                    statModifiers.health,
                    statModifiers.mana,
                    statModifiers.armor,
                    statModifiers.damage,
                    statModifiers.ccduration == -1 ? 1.0f : statModifiers.ccduration
        );
    }

    return statModifiers;

}

float getDefaultMultiplier(Map* map, AutoBalanceInflectionPointSettings inflectionPointSettings)
{
    // You can visually see the effects of this function by using this spreadsheet:
    // https://docs.google.com/spreadsheets/d/100cmKIJIjCZ-ncWd0K9ykO8KUgwFTcwg4h2nfE_UeCc/copy

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

World_Multipliers getWorldMultiplier(Map* map, BaseValueType baseValueType)
{
    World_Multipliers worldMultipliers;

    // null check
    if (!map)
    {
        return worldMultipliers;
    }

    // if this isn't a dungeon, return defaults
    if (!(map->IsDungeon()))
    {
        return worldMultipliers;
    }

    // grab map data
    AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

    // if the map isn't enabled, return defaults
    if (!mapABInfo->enabled)
    {
        return worldMultipliers;
    }

    // if there are no players on the map, return defaults
    if (mapABInfo->allMapPlayers.size() == 0)
    {
        return worldMultipliers;
    }

    // if creatures haven't been counted yet, return defaults
    if (mapABInfo->avgCreatureLevel == 0)
    {
        return worldMultipliers;
    }

    // create some data variables
    InstanceMap* instanceMap = map->ToInstanceMap();
    uint8 avgCreatureLevelRounded = (uint8)(mapABInfo->avgCreatureLevel + 0.5f);

    // get the inflection point settings for this map
    AutoBalanceInflectionPointSettings inflectionPointSettings = getInflectionPointSettings(instanceMap);

    // Generate the default multiplier before level scaling
    // This value is only based on the adjusted number of players in the instance
    float worldMultiplier = 1.0f;
    float defaultMultiplier = getDefaultMultiplier(map, inflectionPointSettings);

    LOG_DEBUG("module.AutoBalance",
        "AutoBalance::getWorldMultiplier: Map {} ({}) {} | defaultMultiplier ({}) = getDefaultMultiplier(map, inflectionPointSettings)",
        map->GetMapName(),
        avgCreatureLevelRounded,
        baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
        defaultMultiplier
    );

    // multiply by the appropriate stat modifiers
    AutoBalanceStatModifiers statModifiers = getStatModifiers(map);

    if (baseValueType == BaseValueType::AUTOBALANCE_HEALTH) // health
    {
        worldMultiplier = defaultMultiplier * statModifiers.global * statModifiers.health;
    }
    else // damage
    {
        worldMultiplier = defaultMultiplier * statModifiers.global * statModifiers.damage;
    }

    LOG_DEBUG("module.AutoBalance",
        "AutoBalance::getWorldMultiplier: Map {} ({}) {} | worldMultiplier ({}) = defaultMultiplier ({}) * statModifiers.global ({}) * statModifiers.{} ({})",
        map->GetMapName(),
        avgCreatureLevelRounded,
        baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
        worldMultiplier,
        defaultMultiplier,
        statModifiers.global,
        baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
        baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? statModifiers.health : statModifiers.damage
    );

    // store the unscaled multiplier
    worldMultipliers.unscaled = worldMultiplier;

    LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}) {} | multiplier before level scaling = ({}).",
            map->GetMapName(),
            avgCreatureLevelRounded,
            baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
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
        LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}) {} | level will be scaled to {}.",
            map->GetMapName(),
            avgCreatureLevelRounded,
            baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
            mapABInfo->worldMultiplierTargetLevel
        );

        // use creature base stats to determine how to level scale the multiplier (the map is a warrior!)
        CreatureBaseStats const* origMapBaseStats = sObjectMgr->GetCreatureBaseStats(avgCreatureLevelRounded, Classes::CLASS_WARRIOR);
        CreatureBaseStats const* adjustedMapBaseStats = sObjectMgr->GetCreatureBaseStats(mapABInfo->worldMultiplierTargetLevel, Classes::CLASS_WARRIOR);

        // Original Base Value
        float originalBaseValue;

        if (baseValueType == BaseValueType::AUTOBALANCE_HEALTH) // health
        {
            originalBaseValue = getBaseExpansionValueForLevel(
                origMapBaseStats->BaseHealth,
                avgCreatureLevelRounded
            );
        }
        else // damage
        {
            originalBaseValue = getBaseExpansionValueForLevel(
                origMapBaseStats->BaseDamage,
                avgCreatureLevelRounded
            );
        }

        LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}) {} | base is {}.",
            map->GetMapName(),
            avgCreatureLevelRounded,
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

        LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}->{}) {} | base is {}.",
            map->GetMapName(),
            avgCreatureLevelRounded,
            mapABInfo->worldMultiplierTargetLevel,
            baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
            newBaseValue
        );

        // update the world multiplier accordingly
        worldMultiplier *= newBaseValue / originalBaseValue;

        LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}->{}) {} | worldMultiplier ({}) = worldMultiplier ({}) * newBaseValue ({}) / originalBaseValue ({})",
            map->GetMapName(),
            mapABInfo->avgCreatureLevel,
            mapABInfo->worldMultiplierTargetLevel,
            baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
            worldMultiplier,
            worldMultiplier,
            newBaseValue,
            originalBaseValue
        );

        LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}->{}) {} | multiplier after level scaling = ({}).",
                map->GetMapName(),
                avgCreatureLevelRounded,
                mapABInfo->worldMultiplierTargetLevel,
                baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
                worldMultiplier
        );
    }
    else
    {
        mapABInfo->worldMultiplierTargetLevel = avgCreatureLevelRounded;

        // level scaling is disabled
        if (!LevelScaling)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}) | not level scaled due to level scaling being disabled. World multiplier target level set to avgCreatureLevel ({}).",
                map->GetMapName(),
                mapABInfo->worldMultiplierTargetLevel,
                mapABInfo->worldMultiplierTargetLevel
            );
        }
        // inside the level skip range
        else
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}) | not level scaled due to being inside the level skip range. World multiplier target level set to avgCreatureLevel ({}).",
                map->GetMapName(),
                mapABInfo->worldMultiplierTargetLevel,
                mapABInfo->worldMultiplierTargetLevel
            );
        }

        LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}) {} | multiplier after level scaling = ({}).",
                map->GetMapName(),
                mapABInfo->worldMultiplierTargetLevel,
                baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
                worldMultiplier
        );
    }

    // store the (potentially) level-scaled multiplier
    worldMultipliers.scaled = worldMultiplier;

    return worldMultipliers;
}

void LoadMapSettings(Map* map)
{
    // Load (or create) the map's info
    AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

    // create an InstanceMap object
    InstanceMap* instanceMap = map->ToInstanceMap();

    LOG_DEBUG("module.AutoBalance", "AutoBalance::LoadMapSettings: Map {} ({}{}, {}-player {}) | Loading settings.",
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
        LOG_WARN("module.AutoBalance", "AutoBalance::LoadMapSettings: Your settings tried to set a minimum player count of {} which is greater than {}'s max player count of {}. Adjusting down.",
            mapABInfo->minPlayers,
            map->GetMapName(),
            instanceMap->GetMaxPlayers()
        );

        mapABInfo->minPlayers = instanceMap->GetMaxPlayers();
    }

    LOG_DEBUG("module.AutoBalance", "AutoBalance::LoadMapSettings: Map {} ({}{}, {}-player {}) | has a minimum player count of {}.",
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
        LOG_ERROR("module.AutoBalance", "AutoBalance::LoadMapSettings: Map {} ({}{}, {}-player {}) | Unable to determine dynamic scaling floor and ceiling for instance.",
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

void AddCreatureToMapCreatureList(Creature* creature, bool addToCreatureList = true, bool forceRecalculation = false)
{
    // make sure we have a creature and that it's assigned to a map
    if (!creature || !creature->GetMap())
        return;

    // if this isn't a dungeon, skip
    if (!(creature->GetMap()->IsDungeon()))
        return;

    // get AutoBalance data
    Map* map = creature->GetMap();
    InstanceMap* instanceMap = map->ToInstanceMap();
    AutoBalanceMapInfo *mapABInfo=instanceMap->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
    AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

    // handle summoned creatures
    if (creature->IsSummon())
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) | is a summon.",
                    creature->GetName(),
                    creatureABInfo->UnmodifiedLevel
        );
        if (creature->ToTempSummon() &&
            creature->ToTempSummon()->GetSummoner() &&
            creature->ToTempSummon()->GetSummoner()->ToCreature())
        {
            creatureABInfo->summoner = creature->ToTempSummon()->GetSummoner()->ToCreature();
            creatureABInfo->summonerName = creatureABInfo->summoner->GetName();
            creatureABInfo->summonerLevel = creatureABInfo->summoner->GetLevel();
            Creature* summoner = creatureABInfo->summoner;

            if (!summoner)
            {
                creatureABInfo->UnmodifiedLevel = mapABInfo->avgCreatureLevel;
                LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) (summon) | is not owned by a summoner. Original level is {}.",
                            creature->GetName(),
                            creatureABInfo->UnmodifiedLevel
                );
            }
            else
            {
                AutoBalanceCreatureInfo *summonerABInfo=summoner->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

                LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) (summon) | is owned by {} ({}).",
                            creature->GetName(),
                            creatureABInfo->UnmodifiedLevel,
                            summoner->GetName(),
                            summonerABInfo->UnmodifiedLevel
                );

                // if the creature or its summoner is a trigger
                if (creature->IsTrigger() || summoner->IsTrigger())
                {
                    LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) (summon) | or their summoner is a trigger.",
                                creature->GetName(),
                                creatureABInfo->UnmodifiedLevel
                    );

                    // if the creature is within the expected level range, allow scaling
                    if (
                        (creatureABInfo->UnmodifiedLevel >= (uint8)(((float)mapABInfo->lfgMinLevel * .85f) + 0.5f)) &&
                        (creatureABInfo->UnmodifiedLevel <= (uint8)(((float)mapABInfo->lfgMaxLevel * 1.15f) + 0.5f))
                    )
                    {
                        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) (summon) | original level is within the expected NPC level for this map ({} to {}). Level scaling is allowed.",
                                    creature->GetName(),
                                    creatureABInfo->UnmodifiedLevel,
                                    (uint8)(((float)mapABInfo->lfgMinLevel * .85f) + 0.5f),
                                    (uint8)(((float)mapABInfo->lfgMaxLevel * 1.15f) + 0.5f)
                        );
                    }
                    else {
                        creatureABInfo->neverLevelScale = true;
                        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) (summon) | original level is outside the expected NPC level for this map ({} to {}). It will keep its original level.",
                                    creature->GetName(),
                                    creatureABInfo->UnmodifiedLevel,
                                    (uint8)(((float)mapABInfo->lfgMinLevel * .85f) + 0.5f),
                                    (uint8)(((float)mapABInfo->lfgMaxLevel * 1.15f) + 0.5f)
                        );
                    }
                }
                // if the creature is not a trigger, match the summoner's level
                else
                {
                    // match the summoner's level
                    creatureABInfo->UnmodifiedLevel = summonerABInfo->UnmodifiedLevel;

                    LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) (summon) | original level will match summoner's level ({}).",
                                creature->GetName(),
                                creatureABInfo->UnmodifiedLevel,
                                summonerABInfo->UnmodifiedLevel
                    );
                }
            }
        }
        // summoned by a player
        else if
        (
            creature->ToTempSummon() &&
            creature->ToTempSummon()->GetSummoner() &&
            creature->ToTempSummon()->GetSummoner()->ToPlayer()
        )
        {
            Player* summoner = creature->ToTempSummon()->GetSummoner()->ToPlayer();

            // is this creature relevant?
            if (isCreatureRelevant(creature))
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) (summon) | is an enemy owned by player {} ({}). Summon original level set to ({}).",
                            creature->GetName(),
                            creatureABInfo->UnmodifiedLevel,
                            summoner->GetName(),
                            summoner->GetLevel(),
                            creatureABInfo->UnmodifiedLevel
                );
            }
            // summon is not relevant
            else
            {
                uint8 newLevel = std::min(summoner->GetLevel(), creature->GetCreatureTemplate()->maxlevel);
                LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) (summon) | is an ally owned by player {} ({}). Summon original level set to ({}) level ({}).",
                            creature->GetName(),
                            creatureABInfo->UnmodifiedLevel,
                            summoner->GetName(),
                            summoner->GetLevel(),
                            newLevel == summoner->GetLevel() ? "player's" : "creature template's max",
                            newLevel
                );
                creatureABInfo->UnmodifiedLevel = newLevel;
            }
        }
        // pets and totems
        else if (creature->IsCreatedByPlayer() || creature->IsPet() || creature->IsHunterPet() || creature->IsTotem())
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) (summon) | is a {}. Original level set to ({}).",
                        creature->GetName(),
                        creatureABInfo->UnmodifiedLevel,
                        creature->IsCreatedByPlayer() ? "creature created by a player" : creature->IsPet() ? "pet" : creature->IsHunterPet() ? "hunter pet" : "totem",
                        creatureABInfo->UnmodifiedLevel
            );
        }
        else
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) (summon) | does not have a summoner. Summon original level set to ({}).",
                        creature->GetName(),
                        creatureABInfo->UnmodifiedLevel,
                        creatureABInfo->UnmodifiedLevel
            );
        }

        // if this is a summon, we shouldn't track it in any list and it does not contribute to the average level
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) (summon) | will not affect the map's stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
        return;
    }
    // handle "special" creatures
    else if (creature->IsCritter() || creature->IsTotem() || creature->IsTrigger())
    {
        // if this is an intentionally-low-level creature (below 85% of the minimum LFG level), leave it where it is
        // if this is an intentionally-high-level creature (above 125% of the maximum LFG level), leave it where it is
        if (
            (creatureABInfo->UnmodifiedLevel < (uint8)(((float)mapABInfo->lfgMinLevel * .85f) + 0.5f)) ||
            (creatureABInfo->UnmodifiedLevel > (uint8)(((float)mapABInfo->lfgMaxLevel * 1.15f) + 0.5f))
        )
        {
            creatureABInfo->neverLevelScale = true;
            LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) (summon) | is a {} and is outside the expected NPC level for this map ({} to {}). Keeping original level of {}.",
                        creature->GetName(),
                        creatureABInfo->UnmodifiedLevel,
                        creature->IsCritter() ? "critter" : creature->IsTotem() ? "totem" : "trigger",
                        (uint8)(((float)mapABInfo->lfgMinLevel * .85f) + 0.5f),
                        (uint8)(((float)mapABInfo->lfgMaxLevel * 1.15f) + 0.5f),
                        creatureABInfo->UnmodifiedLevel
            );
        }
        // otherwise, set it to the target level of the instance so it will get scaled properly
        else
        {
            creatureABInfo->UnmodifiedLevel = mapABInfo->lfgTargetLevel;
            LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) (summon) | is a {} and is within the expected NPC level for this map ({} to {}). Keeping original level of {}.",
                        creature->GetName(),
                        creatureABInfo->UnmodifiedLevel,
                        creature->IsCritter() ? "critter" : creature->IsTotem() ? "totem" : "trigger",
                        (uint8)(((float)mapABInfo->lfgMinLevel * .85f) + 0.5f),
                        (uint8)(((float)mapABInfo->lfgMaxLevel * 1.15f) + 0.5f),
                        creatureABInfo->UnmodifiedLevel
            );

        }

    }
    // creature isn't a summon, just store their unmodified level
    else
    {
        creatureABInfo->UnmodifiedLevel = creatureABInfo->UnmodifiedLevel;
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) | Original level set to ({}).",
            creature->GetName(),
            creatureABInfo->UnmodifiedLevel,
            creatureABInfo->UnmodifiedLevel
        );
    }

    // if this is a creature controlled by the player, skip for stats
    if (((creature->IsHunterPet() || creature->IsPet() || creature->IsSummon()) && creature->IsControlledByPlayer()))
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) | is controlled by the player and will not affect the map's stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
        return;
    }

    // if this is a non-relevant creature, skip for stats
    if (creature->IsCritter() || creature->IsTotem() || creature->IsTrigger())
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) | is a {} and will not affect the map's stats.",
            creature->GetName(),
            creatureABInfo->UnmodifiedLevel,
            creature->IsCritter() ? "critter" : creature->IsTotem() ? "totem" : "trigger"
        );
        return;
    }

    // if the creature level is below 85% of the minimum LFG level, assume it's a flavor creature and shouldn't be tracked
    if (creatureABInfo->UnmodifiedLevel < (uint8)(((float)mapABInfo->lfgMinLevel * 0.85f) + 0.5f))
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) | is below 85% of the LFG min level of {} and will not affect the map's stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel, mapABInfo->lfgMinLevel);
        return;
    }

    // if the creature level is above 125% of the maximum LFG level, assume it's a flavor creature or holiday boss and shouldn't be tracked
    if (creatureABInfo->UnmodifiedLevel > (uint8)(((float)mapABInfo->lfgMaxLevel * 1.15f) + 0.5f))
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) | is above 115% of the LFG max level of {} and will not affect the map's stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel, mapABInfo->lfgMaxLevel);
        return;
    }

    // is this creature already in the map's creature list?
    bool isCreatureAlreadyInCreatureList = creatureABInfo->isInCreatureList;

    // add the creature to the map's creature list if configured to do so
    if (addToCreatureList && !isCreatureAlreadyInCreatureList)
    {
        mapABInfo->allMapCreatures.push_back(creature);
        creatureABInfo->isInCreatureList = true;
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) | is #{} in the creature list.", creature->GetName(), creatureABInfo->UnmodifiedLevel, mapABInfo->allMapCreatures.size());
    }

    // alter stats for the map if needed
    bool isIncludedInMapStats = true;

    // if this creature was already in the creature list, don't consider it for map stats (again)
    // exception for if forceRecalculation is true (used on player enter/exit to recalculate map stats)
    if (isCreatureAlreadyInCreatureList && !forceRecalculation)
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) | is already included in map stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel);

        // ensure that this creature is marked active
        creatureABInfo->isActive = true;

        // increment the active creature counter
        mapABInfo->activeCreatureCount++;

        return;
    }

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
                (!isBossOrBossSummon(creature))
            )
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) | is a a vendor, trainer, or is otherwise not attackable - do not include in map stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            isIncludedInMapStats = false;
        }
        else
        {
            // if the creature is friendly to a player, don't use it to update map stats
            for (std::vector<Player*>::const_iterator playerIterator = mapABInfo->allMapPlayers.begin(); playerIterator != mapABInfo->allMapPlayers.end(); ++playerIterator)
            {
                Player* thisPlayer = *playerIterator;

                // if this player is a Game Master, skip
                if (thisPlayer->IsGameMaster())
                {
                    continue;
                }

                // if the creature is friendly and not a boss
                if (creature->IsFriendlyTo(thisPlayer) && !isBossOrBossSummon(creature))
                {
                    LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) | is friendly to {} - do not include in map stats.",
                        creature->GetName(),
                        creatureABInfo->UnmodifiedLevel,
                        thisPlayer->GetName()
                    );
                    isIncludedInMapStats = false;
                    break;
                }
            }

            // perform the distance check if an override is configured for this map
            if (hasLevelScalingDistanceCheckOverride(instanceMap->GetId()))
            {
                uint32 distance = levelScalingDistanceCheckOverrides[instanceMap->GetId()];
                bool isPlayerWithinDistance = false;

                for (std::vector<Player*>::const_iterator playerIterator = mapABInfo->allMapPlayers.begin(); playerIterator != mapABInfo->allMapPlayers.end(); ++playerIterator)
                {
                    Player* thisPlayer = *playerIterator;

                    // if this player is a Game Master, skip
                    if (thisPlayer->IsGameMaster())
                    {
                        continue;
                    }

                    if (thisPlayer->IsWithinDist(creature, 500))
                    {
                        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) | is in range ({} world units) of player {} and is considered active.", creature->GetName(), creatureABInfo->UnmodifiedLevel, distance, thisPlayer->GetName());
                        isPlayerWithinDistance = true;
                        break;
                    }
                    else
                    {
                        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) | is NOT in range ({} world units) of any player and is NOT considered active.",
                            creature->GetName(),
                            creatureABInfo->UnmodifiedLevel,
                            distance
                        );
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
        float oldAvgCreatureLevel = mapABInfo->avgCreatureLevel;
        float newAvgCreatureLevel = (((float)mapABInfo->avgCreatureLevel * creatureCount) + (float)creatureABInfo->UnmodifiedLevel) / (creatureCount + 1.0f);

        mapABInfo->avgCreatureLevel = newAvgCreatureLevel;

        // increment the active creature counter
        mapABInfo->activeCreatureCount++;

        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: Creature {} ({}) | is included in map stats (active), adjusting avgCreatureLevel to ({})", creature->GetName(), creatureABInfo->UnmodifiedLevel, newAvgCreatureLevel);

        // if the average creature level transitions from one whole number to the next, reset the map's config time so it will refresh
        if (round(oldAvgCreatureLevel) != round(newAvgCreatureLevel))
        {
            mapABInfo->mapConfigTime = 1;
            LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: {} ({}{}) | average creature level changes {}->{}. Force map update. {} ({}{}) map config set to ({}).",
                instanceMap->GetMapName(),
                instanceMap->GetId(),
                instanceMap->GetInstanceId() ? "-" + std::to_string(instanceMap->GetInstanceId()) : "",
                round(oldAvgCreatureLevel),
                round(newAvgCreatureLevel),
                map->GetMapName(),
                map->GetId(),
                map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                mapABInfo->mapConfigTime
            );
        }

        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapCreatureList: There are ({}) creatures included (active) in map stats.", mapABInfo->activeCreatureCount);
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
                LOG_DEBUG("module.AutoBalance", "AutoBalance::RemoveCreatureFromMapData: Creature {} ({}) | is in the creature list and will be removed. There are {} creatures left.", creature->GetName(), creature->GetLevel(), mapABInfo->allMapCreatures.size() - 1);
                mapABInfo->allMapCreatures.erase(creatureIteration);

                // mark this creature as removed
                AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");
                creatureABInfo->isInCreatureList = false;

                // decrement the active creature counter if they were considered active
                if (creatureABInfo->isActive)
                {
                    LOG_DEBUG("module.AutoBalance", "AutoBalance::RemoveCreatureFromMapData: Creature {} ({}) | is no longer active. There are {} active creatures left.",
                        creature->GetName(),
                        creature->GetLevel(),
                        mapABInfo->activeCreatureCount - 1
                    );

                    if (mapABInfo->activeCreatureCount > 0)
                    {
                        mapABInfo->activeCreatureCount--;
                    }
                    else
                    {
                        LOG_DEBUG("module.AutoBalance", "AutoBalance::RemoveCreatureFromMapData: Map {} ({}{}) | activeCreatureCount is already 0. This should not happen.",
                            creature->GetMap()->GetMapName(),
                            creature->GetMap()->GetId(),
                            creature->GetMap()->GetInstanceId() ? "-" + std::to_string(creature->GetMap()->GetInstanceId()) : ""
                        );
                    }
                }

                break;
            }
        }
    }
}

void UpdateMapPlayerStats(Map* map)
{
    // if this isn't a dungeon instance, just bail out immediately
    if (!map->IsDungeon() || !map->GetInstanceId())
    {
        return;
    }

    // get the map's info
    AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
    InstanceMap* instanceMap = map->ToInstanceMap();

    // remember some values
    uint8 oldPlayerCount = mapABInfo->playerCount;
    uint8 oldAdjustedPlayerCount = mapABInfo->adjustedPlayerCount;

    LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapPlayerStats: Map {} ({}{}) | oldPlayerCount = ({}), oldAdjustedPlayerCount = ({}).",
        instanceMap->GetMapName(),
        instanceMap->GetId(),
        instanceMap->GetInstanceId() ? "-" + std::to_string(instanceMap->GetInstanceId()) : "",
        oldPlayerCount,
        oldAdjustedPlayerCount
    );

    // update the player count
    // minimum of 1 to prevent scaling weirdness when only GMs are in the instnace
    mapABInfo->playerCount = mapABInfo->allMapPlayers.size() ? mapABInfo->allMapPlayers.size() : 1;

    LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapPlayerStats: Map {} ({}{}) | playerCount = ({}).",
        instanceMap->GetMapName(),
        instanceMap->GetId(),
        instanceMap->GetInstanceId() ? "-" + std::to_string(instanceMap->GetInstanceId()) : "",
        mapABInfo->playerCount
    );

    LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapPlayerStats: Map {} ({}{}) | combatLocked = ({}), combatLockMinPlayers = ({}).",
        instanceMap->GetMapName(),
        instanceMap->GetId(),
        instanceMap->GetInstanceId() ? "-" + std::to_string(instanceMap->GetInstanceId()) : "",
        mapABInfo->combatLocked,
        mapABInfo->combatLockMinPlayers
    );

    uint8 adjustedPlayerCount = 0;

    // if combat is locked and the new player count is higher than the combat lock, update the combat lock
    if
    (
        mapABInfo->combatLocked &&
        mapABInfo->playerCount > oldPlayerCount &&
        mapABInfo->playerCount > mapABInfo->combatLockMinPlayers
    )
    {
        // start with the actual player count
        adjustedPlayerCount = mapABInfo->playerCount;

        // this is the new floor
        mapABInfo->combatLockMinPlayers = mapABInfo->playerCount;

        LOG_DEBUG("module.AutoBalance_CombatLocking", "AutoBalance::UpdateMapPlayerStats: Map {} ({}{}) | Combat is locked. Combat floor increased. New floor is ({}).",
            instanceMap->GetMapName(),
            instanceMap->GetId(),
            instanceMap->GetInstanceId() ? "-" + std::to_string(instanceMap->GetInstanceId()) : "",
            mapABInfo->combatLockMinPlayers
        );

    }
    // if combat is otherwise locked
    else if (mapABInfo->combatLocked)
    {
        // start with the saved floor
        adjustedPlayerCount = mapABInfo->combatLockMinPlayers ? mapABInfo->combatLockMinPlayers : mapABInfo->playerCount;

        LOG_DEBUG("module.AutoBalance_CombatLocking", "AutoBalance::UpdateMapPlayerStats: Map {} ({}{}) | Combat is locked. Combat floor is ({}).",
            instanceMap->GetMapName(),
            instanceMap->GetId(),
            instanceMap->GetInstanceId() ? "-" + std::to_string(instanceMap->GetInstanceId()) : "",
            mapABInfo->combatLockMinPlayers
        );
    }
    // if combat is not locked
    else
    {
        // start with the actual player count
        adjustedPlayerCount = mapABInfo->playerCount;
    }

    // if the adjusted player count is below the min players setting, adjust it
    if (adjustedPlayerCount < mapABInfo->minPlayers)
        adjustedPlayerCount = mapABInfo->minPlayers;

    // adjust by the PlayerDifficultyOffset
    adjustedPlayerCount += PlayerCountDifficultyOffset;

    // store the adjusted player count in the map's info
    mapABInfo->adjustedPlayerCount = adjustedPlayerCount;

    // if the adjustedPlayerCount changed, schedule this map for a reconfiguration
    if (oldAdjustedPlayerCount != mapABInfo->adjustedPlayerCount)
    {
        mapABInfo->mapConfigTime = 1;
        LOG_DEBUG("module.AutoBalance_CombatLocking", "AutoBalance::UpdateMapPlayerStats: Map {} ({}{}) | Player difficulty changes ({}->{}). Force map update. {} ({}{}) map config time set to ({}).",
            instanceMap->GetMapName(),
            instanceMap->GetId(),
            instanceMap->GetInstanceId() ? "-" + std::to_string(instanceMap->GetInstanceId()) : "",
            oldAdjustedPlayerCount,
            mapABInfo->adjustedPlayerCount,
            instanceMap->GetMapName(),
            instanceMap->GetId(),
            instanceMap->GetInstanceId() ? "-" + std::to_string(instanceMap->GetInstanceId()) : "",
            mapABInfo->mapConfigTime
        );
    }

    uint8 highestPlayerLevel = 0;
    uint8 lowestPlayerLevel = 80;

    // iterate through the players and update the highest and lowest player levels
    for (std::vector<Player*>::const_iterator playerIterator = mapABInfo->allMapPlayers.begin(); playerIterator != mapABInfo->allMapPlayers.end(); ++playerIterator)
    {
        Player* thisPlayer = *playerIterator;

        if (thisPlayer && !thisPlayer->IsGameMaster())
        {
            if (thisPlayer->GetLevel() > highestPlayerLevel || highestPlayerLevel == 0)
            {
                highestPlayerLevel = thisPlayer->GetLevel();
            }

            if (thisPlayer->GetLevel() < lowestPlayerLevel || lowestPlayerLevel == 0)
            {
                lowestPlayerLevel = thisPlayer->GetLevel();
            }
        }
    }

    mapABInfo->highestPlayerLevel = highestPlayerLevel;
    mapABInfo->lowestPlayerLevel = lowestPlayerLevel;

    if (!highestPlayerLevel)
    {
        mapABInfo->highestPlayerLevel = mapABInfo->lfgTargetLevel;
        mapABInfo->lowestPlayerLevel = mapABInfo->lfgTargetLevel;

        // no non-GM players on the map
        LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapPlayerStats: Map {} ({}{}, {}-player {}) | has no non-GM players. Player stats derived from LFG target level.",
            map->GetMapName(),
            map->GetId(),
            map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
            instanceMap->GetMaxPlayers(),
            instanceMap->IsHeroic() ? "Heroic" : "Normal"
        );
    }
    else
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapPlayerStats: Map {} ({}{}, {}-player {}) | has {} player(s) with level range ({})-({}). Difficulty is {} player(s).",
            map->GetMapName(),
            map->GetId(),
            map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
            instanceMap->GetMaxPlayers(),
            instanceMap->IsHeroic() ? "Heroic" : "Normal",
            mapABInfo->playerCount,
            mapABInfo->lowestPlayerLevel,
            mapABInfo->highestPlayerLevel,
            mapABInfo->adjustedPlayerCount
        );
    }
}

void AddPlayerToMap(Map* map, Player* player)
{
    // get map data
    AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");


    if (!player)
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddPlayerToMap: Map {} ({}{}) | Player does not exist.",
            map->GetMapName(),
            map->GetId(),
            map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
        );
        return;
    }
    // player is a GM
    else if (player->IsGameMaster())
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddPlayerToMap: Map {} ({}{}) | Game Master ({}) will not be added to the player list.",
            map->GetMapName(),
            map->GetId(),
            map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
            player->GetName()
        );
        return;
    }
    // if this player is already in the map's player list, skip
    else if (std::find(mapABInfo->allMapPlayers.begin(), mapABInfo->allMapPlayers.end(), player) != mapABInfo->allMapPlayers.end())
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddPlayerToMap: Player {} ({}) | is already in the map's player list.",
            player->GetName(),
            player->GetLevel()
        );
        return;
    }

    // add the player to the map's player list
    mapABInfo->allMapPlayers.push_back(player);
    LOG_DEBUG("module.AutoBalance", "AutoBalance::AddPlayerToMap: Player {} ({}) | added to the map's player list.", player->GetName(), player->GetLevel());

    // update the map's player stats
    UpdateMapPlayerStats(map);
}

bool RemovePlayerFromMap(Map* map, Player* player)
{
    // get map data
    AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

    // if this player isn't in the map's player list, skip
    if (std::find(mapABInfo->allMapPlayers.begin(), mapABInfo->allMapPlayers.end(), player) == mapABInfo->allMapPlayers.end())
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::RemovePlayerFromMap: Player {} ({}) | was not in the map's player list.", player->GetName(), player->GetLevel());
        return false;
    }

    // remove the player from the map's player list
    mapABInfo->allMapPlayers.erase(std::remove(mapABInfo->allMapPlayers.begin(), mapABInfo->allMapPlayers.end(), player), mapABInfo->allMapPlayers.end());
    LOG_DEBUG("module.AutoBalance", "AutoBalance::RemovePlayerFromMap: Player {} ({}) | removed from the map's player list.", player->GetName(), player->GetLevel());

    // if the map is combat locked, schedule a map update for when combat ends
    if (mapABInfo->combatLocked)
    {
        mapABInfo->combatLockTripped = true;
    }

    // update the map's player stats
    UpdateMapPlayerStats(map);

    return true;
}

bool UpdateMapDataIfNeeded(Map* map, bool force = false)
{
    // get map data
    AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

    // if map needs update
    if (force || mapABInfo->globalConfigTime < globalConfigTime || mapABInfo->mapConfigTime < mapABInfo->globalConfigTime)
    {

        LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded: Map {} ({}{}) | globalConfigTime = ({}) | mapConfigTime = ({})",
                    map->GetMapName(),
                    map->GetId(),
                    map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                    mapABInfo->globalConfigTime,
                    mapABInfo->mapConfigTime
        );

        // update forced
        if (force)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded: Map {} ({}{}) | Update forced.",
                        map->GetMapName(),
                        map->GetId(),
                        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                        mapABInfo->mapConfigTime
            );
        }

        // some tracking variables
        bool isGlobalConfigOutOfDate = mapABInfo->globalConfigTime < globalConfigTime;
        bool isMapConfigOutOfDate = mapABInfo->mapConfigTime < globalConfigTime;

        // if this was triggered by a global config update, redetect players
        if (isGlobalConfigOutOfDate)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded: Map {} ({}{}) | Global config is out of date ({} < {}) and will be updated.",
                        map->GetMapName(),
                        map->GetId(),
                        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                        mapABInfo->globalConfigTime,
                        globalConfigTime
            );

            LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded: Map {} ({}{}) | Will recount players in the map.",
                        map->GetMapName(),
                        map->GetId(),
                        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
            );

            // clear the map's player list
            mapABInfo->allMapPlayers.clear();

            // reset the combat lock
            mapABInfo->combatLockMinPlayers = 0;

            // get the map's player list
            Map::PlayerList const &playerList = map->GetPlayers();

            // re-count the players in the dungeon
            for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
            {
                Player* thisPlayer = playerIteration->GetSource();

                // if the player is in combat, combat lock the map
                if (thisPlayer->IsInCombat())
                {
                    mapABInfo->combatLocked = true;
                    LOG_DEBUG("module.AutoBalance_CombatLocking", "AutoBalance::UpdateMapDataIfNeeded: Map {} ({}{}) | Player {} is in combat. Map is combat locked.",
                        map->GetMapName(),
                        map->GetId(),
                        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                        thisPlayer->GetName()
                    );
                }

                // (conditionally) add the player to the map's player list
                AddPlayerToMap(map, thisPlayer);
            }

            // map's player count will be updated in UpdateMapPlayerStats below
        }

        // map config is out of date
        if (isMapConfigOutOfDate)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded: Map {} ({}{}) | Map config is out of date ({} < {}) and will be updated.",
                        map->GetMapName(),
                        map->GetId(),
                        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                        mapABInfo->mapConfigTime,
                        globalConfigTime
            );
        }

        // should the map be enabled?
        bool newEnabled = ShouldMapBeEnabled(map);

        // if this is a transition between enabled states, reset the map's config time so it will refresh
        if (mapABInfo->enabled != newEnabled)
        {
            mapABInfo->mapConfigTime = 1;

            LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded: Map {} ({}{}) | Enabled state transitions from {}->{}, map update forced. Map config time set to ({}).",
                map->GetMapName(),
                map->GetId(),
                map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                mapABInfo->enabled ? "ENABLED" : "DISABLED",
                newEnabled ? "ENABLED" : "DISABLED",
                mapABInfo->mapConfigTime
            );
        }

        // update the enabled state
        mapABInfo->enabled = newEnabled;

        if (!mapABInfo->enabled)
        {
            // mark the config updated to prevent checking the disabled map repeatedly
            mapABInfo->globalConfigTime = globalConfigTime;
            mapABInfo->mapConfigTime = globalConfigTime;

            LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded: Map {} ({}{}) | is disabled.",
                map->GetMapName(),
                map->GetId(),
                map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
            );
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
            mapABInfo->prevMapLevel = mapABInfo->mapLevel;
            mapABInfo->mapLevel = (uint8)(mapABInfo->avgCreatureLevel + 0.5f);
            mapABInfo->isLevelScalingEnabled = false;

            // only log if the mapLevel has changed
            if (mapABInfo->prevMapLevel != mapABInfo->mapLevel)
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded: Map {} ({}{}, {}-player {}) | Level scaling is disabled. Map level tracking stat updated {}{} (original level).",
                    map->GetMapName(),
                    map->GetId(),
                    map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                    map->ToInstanceMap()->GetMaxPlayers(),
                    map->ToInstanceMap()->IsHeroic() ? "Heroic" : "Normal",
                    mapABInfo->mapLevel != mapABInfo->prevMapLevel ? std::to_string(mapABInfo->prevMapLevel) + "->" : "",
                    mapABInfo->mapLevel
                );
            }

        }
        // If the average creature level is lower than the highest player level,
        // set the map level to the average creature level, rounded to the nearest integer
        else if (mapABInfo->avgCreatureLevel <= mapABInfo->highestPlayerLevel)
        {
            mapABInfo->prevMapLevel = mapABInfo->mapLevel;
            mapABInfo->mapLevel = (uint8)(mapABInfo->avgCreatureLevel + 0.5f);
            mapABInfo->isLevelScalingEnabled = true;

            // only log if the mapLevel has changed
            if (mapABInfo->prevMapLevel != mapABInfo->mapLevel)
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded: Map {} ({}{}, {}-player {}) | Level scaling is enabled. Map level updated ({}{}) (average creature level).",
                    map->GetMapName(),
                    map->GetId(),
                    map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                    map->ToInstanceMap()->GetMaxPlayers(),
                    map->ToInstanceMap()->IsHeroic() ? "Heroic" : "Normal",
                    mapABInfo->mapLevel != mapABInfo->prevMapLevel ? std::to_string(mapABInfo->prevMapLevel) + "->" : "",
                    mapABInfo->mapLevel
                );
            }
        }
        // caps at the highest player level
        else
        {
            mapABInfo->prevMapLevel = mapABInfo->mapLevel;
            mapABInfo->mapLevel = mapABInfo->highestPlayerLevel;
            mapABInfo->isLevelScalingEnabled = true;

            // only log if the mapLevel has changed
            if (mapABInfo->prevMapLevel != mapABInfo->mapLevel)
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded: Map {} ({}{}, {}-player {}) | Lcaling is enabled. Map level updated ({}{}) (highest player level).",
                    map->GetMapName(),
                    map->GetId(),
                    map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                    map->ToInstanceMap()->GetMaxPlayers(),
                    map->ToInstanceMap()->IsHeroic() ? "Heroic" : "Normal",
                    mapABInfo->mapLevel != mapABInfo->prevMapLevel ? std::to_string(mapABInfo->prevMapLevel) + "->" : "",
                    mapABInfo->mapLevel
                );
            }
        }

        // World multipliers only need to be updated if the mapLevel has changed OR if the map config is out of date
        if (mapABInfo->prevMapLevel != mapABInfo->mapLevel || isMapConfigOutOfDate)
        {
            // Update World Health multiplier
            // Used for scaling damage against destructible game objects
            World_Multipliers health = getWorldMultiplier(map, BaseValueType::AUTOBALANCE_HEALTH);
            mapABInfo->worldHealthMultiplier = health.unscaled;

            // Update World Damage or Healing multiplier
            // Used for scaling damage and healing between players and/or non-creatures
            World_Multipliers damageHealing = getWorldMultiplier(map, BaseValueType::AUTOBALANCE_DAMAGE_HEALING);
            mapABInfo->worldDamageHealingMultiplier = damageHealing.unscaled;
            mapABInfo->scaledWorldDamageHealingMultiplier = damageHealing.scaled;
        }

        // mark the config updated
        mapABInfo->globalConfigTime = globalConfigTime;
        mapABInfo->mapConfigTime = GetCurrentConfigTime();

        LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded: {} ({}{}) | Global config time set to ({}).",
                    map->GetMapName(),
                    map->GetId(),
                    map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                    mapABInfo->globalConfigTime
        );

        LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded: {} ({}{}) | Map config time set to ({}).",
                    map->GetMapName(),
                    map->GetId(),
                    map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                    mapABInfo->mapConfigTime
        );

        return true;
    }
    else
    {
        // LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded: Map {} ({}{}) global config is up to date ({} == {}).",
        //             map->GetMapName(),
        //             map->GetId(),
        //             map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
        //             mapABInfo->globalConfigTime,
        //             globalConfigTime
        // );

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
        globalConfigTime = GetCurrentConfigTime();

        LOG_INFO("module.AutoBalance", "AutoBalance::OnBeforeConfigLoad: Config loaded. Global config time set to ({}).", globalConfigTime);
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
        Enable10MHeroic = sConfigMgr->GetOption<bool>("AutoBalance.Enable.10MHeroic", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false));
        Enable25MHeroic = sConfigMgr->GetOption<bool>("AutoBalance.Enable.25MHeroic", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false));
        EnableOtherHeroic = sConfigMgr->GetOption<bool>("AutoBalance.Enable.OtherHeroic", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false));

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
        if (LevelScalingEndGameBoost)
        {
            LOG_WARN("server.loading", "mod-autobalance: `AutoBalance.LevelScaling.EndGameBoost` is enabled in the configuration, but is not currently implemented. No effect.");
            LevelScalingEndGameBoost = 0;
        }

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

            LOG_DEBUG("module.AutoBalance", "AutoBalance_PlayerScript::OnLevelChanged: {} has leveled ({}->{})", player->GetName(), oldlevel, player->GetLevel());
            if (!player || player->IsGameMaster())
            {
                return;
            }

            Map* map = player->GetMap();

            if (!map || !map->IsDungeon())
            {
                return;
            }

            // update the map's player stats
            UpdateMapPlayerStats(map);

            // schedule all creatures for an update
            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
            mapABInfo->mapConfigTime = GetCurrentConfigTime();
        }

        void OnGiveXP(Player* player, uint32& amount, Unit* victim, uint8 /*xpSource*/) override
        {
            Map* map = player->GetMap();

            // If this isn't a dungeon, make no changes
            if (!map->IsDungeon() || !map->GetInstanceId() || !victim)
            {
                return;
            }

            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            if (victim && RewardScalingXP && mapABInfo->enabled)
            {
                Map* map = player->GetMap();

                AutoBalanceCreatureInfo *creatureABInfo=victim->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

                if (map->IsDungeon())
                {
                    if (RewardScalingMethod == AUTOBALANCE_SCALING_DYNAMIC)
                    {
                        LOG_DEBUG("module.AutoBalance", "AutoBalance_PlayerScript::OnGiveXP: Distributing XP from '{}' to '{}' in dynamic mode - {}->{}",
                                 victim->GetName(), player->GetName(), amount, uint32(amount * creatureABInfo->XPModifier));
                        amount = uint32(amount * creatureABInfo->XPModifier);
                    }
                    else if (RewardScalingMethod == AUTOBALANCE_SCALING_FIXED)
                    {
                        // Ensure that the players always get the same XP, even when entering the dungeon alone
                        auto maxPlayerCount = map->ToInstanceMap()->GetMaxPlayers();
                        auto currentPlayerCount = mapABInfo->playerCount;
                        LOG_DEBUG("module.AutoBalance", "AutoBalance_PlayerScript::OnGiveXP: Distributing XP from '{}' to '{}' in fixed mode - {}->{}",
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
                        LOG_DEBUG("module.AutoBalance", "AutoBalance_PlayerScript::OnBeforeLootMoney: Distributing money from '{}' in dynamic mode - {}->{}",
                                 sourceCreature->GetName(), loot->gold, uint32(loot->gold * creatureABInfo->MoneyModifier));
                        loot->gold = uint32(loot->gold * creatureABInfo->MoneyModifier);
                    }
                    // Fixed Mode
                    else if (RewardScalingMethod == AUTOBALANCE_SCALING_FIXED)
                    {
                        // Ensure that the players always get the same money, even when entering the dungeon alone
                        auto maxPlayerCount = map->ToInstanceMap()->GetMaxPlayers();
                        auto currentPlayerCount = mapABInfo->playerCount;
                        LOG_DEBUG("module.AutoBalance", "AutoBalance_PlayerScript::OnBeforeLootMoney: Distributing money from '{}' in fixed mode - {}->{}",
                                 sourceCreature->GetName(), loot->gold, uint32(loot->gold * creatureABInfo->MoneyModifier * ((float)currentPlayerCount / maxPlayerCount)));
                        loot->gold = uint32(loot->gold * creatureABInfo->MoneyModifier * ((float)currentPlayerCount / maxPlayerCount));
                    }
                }
                // for all other loot sources, just distribute in Fixed mode as though the instance was full
                else
                {
                    auto maxPlayerCount = map->ToInstanceMap()->GetMaxPlayers();
                    auto currentPlayerCount = mapABInfo->playerCount;
                    LOG_DEBUG("module.AutoBalance", "AutoBalance_PlayerScript::OnBeforeLootMoney: Distributing money from a non-creature in fixed mode - {}->{}",
                             loot->gold, uint32(loot->gold * ((float)currentPlayerCount / maxPlayerCount)));
                    loot->gold = uint32(loot->gold * ((float)currentPlayerCount / maxPlayerCount));
                }
            }
        }

        virtual void OnPlayerEnterCombat(Player* player, Unit* /*enemy*/) override
        {
            // if the player or their map is gone, return
            if (!player || !player->GetMap())
            {
                return;
            }

            Map* map = player->GetMap();

            // If this isn't a dungeon, no work to do
            if (!map || !map->IsDungeon())
            {
                return;
            }

            LOG_DEBUG("module.AutoBalance_CombatLocking", "AutoBalance_PlayerScript::OnPlayerEnterCombat: {} enters combat.", player->GetName());

            AutoBalanceMapInfo *mapABInfo = map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            // if this map isn't enabled, no work to do
            if (!mapABInfo->enabled)
            {
                return;
            }

            // lock the current map
            if (!mapABInfo->combatLocked)
            {
                mapABInfo->combatLocked = true;
                mapABInfo->combatLockMinPlayers = mapABInfo->playerCount;

                LOG_DEBUG("module.AutoBalance_CombatLocking", "AutoBalance_PlayerScript::OnPlayerEnterCombat: Map {} ({}{}) | Locking difficulty to no less than ({}) as {} enters combat.",
                            map->GetMapName(),
                            map->GetId(),
                            map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                            mapABInfo->combatLockMinPlayers,
                            player->GetName()
                );
            }
        }

        virtual void OnPlayerLeaveCombat(Player* player) override
        {
            // if the player or their map is gone, return
            if (!player || !player->GetMap())
            {
                return;
            }

            Map* map = player->GetMap();

            // If this isn't a dungeon, no work to do
            if (!map || !map->IsDungeon())
            {
                return;
            }

            // this hook can get called even if the player isn't in combat
            // I believe this happens whenever AC attempts to remove combat, but it doesn't check to see if the player is in combat first
            // unfortunately, `player->IsInCombat()` doesn't work here
            LOG_DEBUG("module.AutoBalance_CombatLocking", "AutoBalance_PlayerScript::OnPlayerLeaveCombat: {} leaves (or wasn't in) combat.", player->GetName());

            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            // if this map isn't enabled, no work to do
            if (!mapABInfo->enabled)
            {
                return;
            }

            // check to see if any of the other players are in combat
            bool anyPlayersInCombat = false;
            for (auto player : mapABInfo->allMapPlayers)
            {
                if (player && player->IsInCombat())
                {
                    anyPlayersInCombat = true;

                    LOG_DEBUG("module.AutoBalance_CombatLocking", "AutoBalance_PlayerScript::OnPlayerLeaveCombat: Map {} ({}{}) | Player {} (and potentially others) are still in combat.",
                                map->GetMapName(),
                                map->GetId(),
                                map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                                player->GetName()
                    );

                    break;
                }
            }

            auto locale = player->GetSession()->GetSessionDbLocaleIndex();

            // if no players are in combat, unlock the map
            if (!anyPlayersInCombat && mapABInfo->combatLocked)
            {
                mapABInfo->combatLocked = false;
                mapABInfo->combatLockMinPlayers = 0;

                LOG_DEBUG("module.AutoBalance_CombatLocking", "AutoBalance_PlayerScript::OnPlayerLeaveCombat: Map {} ({}{}) | Unlocking difficulty as {} leaves combat.",
                            map->GetMapName(),
                            map->GetId(),
                            map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                            player->GetName()
                );

                // if the combat lock needed to be used, notify the players of it lifting
                if (mapABInfo->combatLockTripped)
                {
                    for (auto player : mapABInfo->allMapPlayers)
                    {
                        if (player && player->GetSession())
                        {
                            ChatHandler(player->GetSession()).PSendSysMessage(ABGetLocaleText(locale, "leaving_instance_combat_change").c_str());
                        }
                    }
                }

                // if the number of players changed while combat was in progress, schedule the map for an update
                if (mapABInfo->combatLockTripped && mapABInfo->playerCount != mapABInfo->combatLockMinPlayers)
                {
                    mapABInfo->mapConfigTime = 1;
                    LOG_DEBUG("module.AutoBalance_CombatLocking", "AutoBalance_PlayerScript::OnPlayerLeaveCombat: Map {} ({}{}) | Reset map config time to ({}).",
                                map->GetMapName(),
                                map->GetId(),
                                map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                                mapABInfo->mapConfigTime
                    );

                    mapABInfo->combatLockTripped = false;
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

            // only debug if the source or target is a player
            bool _debug_damage_and_healing = ((source && (source->GetTypeId() == TYPEID_PLAYER || source->IsControlledByPlayer())) || (target && target->GetTypeId() == TYPEID_PLAYER));
            _debug_damage_and_healing = (source && source->GetMap()->GetInstanceId());

            if (_debug_damage_and_healing) _Debug_Output("ModifyPeriodicDamageAurasTick", target, source, adjustedAmount, AUTOBALANCE_DAMAGE_HEALING_DEBUG_PHASE_BEFORE, spellInfo->SpellName[0], spellInfo->Id);

            // set amount to the absolute value of the function call
            // the provided amount doesn't indicate whether it's a positive or negative value
            adjustedAmount = _Modify_Damage_Healing(target, source, adjustedAmount, spellInfo);
            amount = abs(adjustedAmount);

            if (_debug_damage_and_healing) _Debug_Output("ModifyPeriodicDamageAurasTick", target, source, adjustedAmount, AUTOBALANCE_DAMAGE_HEALING_DEBUG_PHASE_AFTER, spellInfo->SpellName[0], spellInfo->Id);
        }

        void ModifySpellDamageTaken(Unit* target, Unit* source, int32& amount, SpellInfo const* spellInfo) override
        {
            // if the spell is negative (damage), we need to flip the sign to negative
            // if the spell is positive (healing or other) we keep it the same (positive)
            int32 adjustedAmount = !spellInfo->IsPositive() ? amount * -1 : amount;

            // only debug if the source or target is a player
            bool _debug_damage_and_healing = ((source && (source->GetTypeId() == TYPEID_PLAYER || source->IsControlledByPlayer())) || (target && target->GetTypeId() == TYPEID_PLAYER));
            _debug_damage_and_healing = (source && source->GetMap()->GetInstanceId());

            if (_debug_damage_and_healing) _Debug_Output("ModifySpellDamageTaken", target, source, adjustedAmount, AUTOBALANCE_DAMAGE_HEALING_DEBUG_PHASE_BEFORE, spellInfo->SpellName[0], spellInfo->Id);

            // set amount to the absolute value of the function call
            // the provided amount doesn't indicate whether it's a positive or negative value
            adjustedAmount = _Modify_Damage_Healing(target, source, adjustedAmount, spellInfo);
            amount = abs(adjustedAmount);

            if (_debug_damage_and_healing) _Debug_Output("ModifySpellDamageTaken", target, source, adjustedAmount, AUTOBALANCE_DAMAGE_HEALING_DEBUG_PHASE_AFTER, spellInfo->SpellName[0], spellInfo->Id);
        }

        void ModifyMeleeDamage(Unit* target, Unit* source, uint32& amount) override
        {
            // melee damage is always negative, so we need to flip the sign to negative
            int32 adjustedAmount = amount * -1;

            // only debug if the source or target is a player
            bool _debug_damage_and_healing = ((source && (source->GetTypeId() == TYPEID_PLAYER || source->IsControlledByPlayer())) || (target && target->GetTypeId() == TYPEID_PLAYER));
            _debug_damage_and_healing = (source && source->GetMap()->GetInstanceId());

            if (_debug_damage_and_healing) _Debug_Output("ModifyMeleeDamage", target, source, adjustedAmount, AUTOBALANCE_DAMAGE_HEALING_DEBUG_PHASE_BEFORE, "Melee");

            // set amount to the absolute value of the function call
            adjustedAmount = _Modify_Damage_Healing(target, source, adjustedAmount);
            amount = abs(adjustedAmount);

            if (_debug_damage_and_healing) _Debug_Output("ModifyMeleeDamage", target, source, adjustedAmount, AUTOBALANCE_DAMAGE_HEALING_DEBUG_PHASE_AFTER, "Melee");
        }

        void ModifyHealReceived(Unit* target, Unit* source, uint32& amount, SpellInfo const* spellInfo) override
        {
            // healing is always positive, no need for any sign flip

            // only debug if the source or target is a player
            bool _debug_damage_and_healing = ((source && (source->GetTypeId() == TYPEID_PLAYER || source->IsControlledByPlayer())) || (target && target->GetTypeId() == TYPEID_PLAYER));
            _debug_damage_and_healing = (source && source->GetMap()->GetInstanceId());

            if (_debug_damage_and_healing) _Debug_Output("ModifyHealReceived", target, source, amount, AUTOBALANCE_DAMAGE_HEALING_DEBUG_PHASE_BEFORE, spellInfo->SpellName[0], spellInfo->Id);

            amount = _Modify_Damage_Healing(target, source, amount, spellInfo);

            if (_debug_damage_and_healing) _Debug_Output("ModifyHealReceived", target, source, amount, AUTOBALANCE_DAMAGE_HEALING_DEBUG_PHASE_AFTER, spellInfo->SpellName[0], spellInfo->Id);
        }

        void OnAuraApply(Unit* unit, Aura* aura) override {
            // only debug if the source or target is a player
            bool _debug_damage_and_healing = (unit && unit->GetTypeId() == TYPEID_PLAYER);
            _debug_damage_and_healing = (unit && unit->GetMap()->GetInstanceId());

            // Only if this aura has a duration
            if (aura && (aura->GetDuration() > 0 || aura->GetMaxDuration() > 0))
            {
                uint32 auraDuration = _Modifier_CCDuration(unit, aura->GetCaster(), aura);

                // only update if we decided to change it
                if (auraDuration != (float)aura->GetDuration())
                {
                    if (_debug_damage_and_healing) LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::OnAuraApply(): Spell '{}' had it's duration adjusted ({}->{}).",
                        aura->GetSpellInfo()->SpellName[0],
                        aura->GetMaxDuration()/1000,
                        auraDuration/1000
                    );

                    aura->SetMaxDuration(auraDuration);
                    aura->SetDuration(auraDuration);
                }
            }
        }

    private:
        [[maybe_unused]] bool _debug_damage_and_healing = false; // defaults to false, overwritten in each function

        void _Debug_Output(std::string function_name, Unit* target, Unit* source, int32 amount, Damage_Healing_Debug_Phase phase, std::string spell_name = "Unknown Spell", uint32 spell_id = 0)
        {
            if (phase == AUTOBALANCE_DAMAGE_HEALING_DEBUG_PHASE_BEFORE)
            {
                LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance:: {}", SPACER);
            }

            if (target && source && amount)
            {
                LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::{}: {}: {}{} {} {}{} with {}{} for ({})",
                    function_name,
                    phase ? "AFTER" : "BEFORE",
                    source->GetName(),
                    source->GetEntry() ? " (" + std::to_string(source->GetEntry()) + ")" : "",
                    amount > 0 ? "heals" : "damages",
                    target->GetName(),
                    target->GetEntry() ? " (" + std::to_string(target->GetEntry()) + ")" : "",
                    spell_name,
                    spell_id ? " (" + std::to_string(spell_id) + ")" : "",
                    amount
                );
            }
            else if (target && source)
            {
                LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::{}: {}: {}{} damages {}{} with {}{} for (0)",
                    function_name,
                    phase ? "AFTER" : "BEFORE",
                    source->GetName(),
                    source->GetEntry() ? " (" + std::to_string(source->GetEntry()) + ")" : "",
                    target->GetName(),
                    target->GetEntry() ? " (" + std::to_string(target->GetEntry()) + ")" : "",
                    spell_name,
                    spell_id ? " (" + std::to_string(spell_id) + ")" : ""
                );
            }
            else if (target && amount)
            {
                LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::{}: {}: ?? {} {}{} with {}{} for ({})",
                    function_name,
                    phase ? "AFTER" : "BEFORE",
                    amount > 0 ? "heals" : "damages",
                    target->GetName(),
                    target->GetEntry() ? " (" + std::to_string(target->GetEntry()) + ")" : "",
                    spell_name,
                    spell_id ? " (" + std::to_string(spell_id) + ")" : "",
                    amount
                );
            }
            else if (target)
            {
                LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::{}: {}: ?? affects {}{} with {}{}",
                    function_name,
                    phase ? "AFTER" : "BEFORE",
                    target->GetName(),
                    target->GetEntry() ? " (" + std::to_string(target->GetEntry()) + ")" : "",
                    spell_name,
                    spell_id ? " (" + std::to_string(spell_id) + ")" : ""
                );
            }
            else
            {
                LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::{}: {}: W? T? F? with {}{}",
                    function_name,
                    phase ? "AFTER" : "BEFORE",
                    spell_name,
                    spell_id ? " (" + std::to_string(spell_id) + ")" : ""
                );
            }
        }

        int32 _Modify_Damage_Healing(Unit* target, Unit* source, int32 amount, SpellInfo const* spellInfo = nullptr)
        {
            //
            // Pre-flight Checks
            //

            // only debug if the source or target is a player
            bool _debug_damage_and_healing = ((source && (source->GetTypeId() == TYPEID_PLAYER || source->IsControlledByPlayer())) || (target && target->GetTypeId() == TYPEID_PLAYER));
            _debug_damage_and_healing = (source && source->GetMap()->GetInstanceId());

            // check that we're enabled globally, else return the original value
            if (!EnableGlobal)
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: EnableGlobal is false, returning original value of ({}).", amount);

                return amount;
            }

            // if the source is gone (logged off? despawned?), use the same target and source.
            // hacky, but better than crashing or having the damage go to 1.0x
            if (!source)
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Source is null, using target as source.");

                source = target;
            }

            // make sure the source and target are in an instance, else return the original damage
            if (!(source->GetMap()->IsDungeon() && target->GetMap()->IsDungeon()))
            {
                //if (_debug_damage_and_healing)
                //    LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Not in an instance, returning original value of ({}).", amount);

                return amount;
            }

            // make sure that the source is in the world, else return the original value
            if (!source->IsInWorld())
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Source does not exist in the world, returning original value of ({}).", amount);

                return amount;
            }

            // if the spell ID is in our "never modify" list, return the original value
            if
            (
                spellInfo &&
                spellInfo->Id &&
                std::find
                (
                    spellIdsToNeverModify.begin(),
                    spellIdsToNeverModify.end(),
                    spellInfo->Id
                ) != spellIdsToNeverModify.end()
            )
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Spell {}({}) is in the never modify list, returning original value of ({}).",
                        spellInfo->SpellName[0],
                        spellInfo->Id,
                        amount
                    );

                return amount;
            }

            // get the maps' info
            AutoBalanceMapInfo *sourceMapABInfo = source->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
            AutoBalanceMapInfo *targetMapABInfo = target->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            // if either the target or the source's maps are not enabled, return the original damage
            if (!sourceMapABInfo->enabled || !targetMapABInfo->enabled)
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Source or Target's map is not enabled, returning original value of ({}).", amount);

                return amount;
            }

            //
            // Source and Target Checking
            //

            // if the source is a player and they are healing themselves, return the original value
            if (source->GetTypeId() == TYPEID_PLAYER && source->GetGUID() == target->GetGUID() && amount >= 0)
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Source is a player that is self-healing, returning original value of ({}).", amount);

                return amount;
            }
            // if the source is a player and they are damaging themselves, log to debug but continue
            else if (source->GetTypeId() == TYPEID_PLAYER && source->GetGUID() == target->GetGUID() && amount < 0)
            {
                // if the spell used is in our list of spells to ignore, return the original value
                if
                (
                    spellInfo &&
                    spellInfo->Id &&
                    std::find
                    (
                        spellIdsThatSpendPlayerHealth.begin(),
                        spellIdsThatSpendPlayerHealth.end(),
                        spellInfo->Id
                    ) != spellIdsThatSpendPlayerHealth.end()
                )
                {
                    if (_debug_damage_and_healing)
                        LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Source is a player that is self-damaging with a spell that is ignored, returning original value of ({}).", amount);

                    return amount;
                }

                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Source is a player that is self-damaging, continuing.");
            }
            // if the source is a player and they are damaging unit that is friendly, log to debug but continue
            else if (source->GetTypeId() == TYPEID_PLAYER && target->IsFriendlyTo(source) && amount < 0)
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Source is a player that is damaging a friendly unit, continuing.");
            }
            // if the source is a player under any other condition, return the original value
            else if (source->GetTypeId() == TYPEID_PLAYER)
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Source is an enemy player, returning original value of ({}).", amount);

                return amount;
            }
            // if the creature is attacking itself with an aura with effect type SPELL_AURA_SHARE_DAMAGE_PCT, return the orginal damage
            else if
            (
                source->GetTypeId() == TYPEID_UNIT &&
                source->GetTypeId() != TYPEID_PLAYER &&
                source->GetGUID() == target->GetGUID() &&
                _isAuraWithEffectType(spellInfo, SPELL_AURA_SHARE_DAMAGE_PCT)
            )
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Source is a creature that is self-damaging with an aura that shares damage, returning original value of ({}).", amount);

                return amount;
            }

            // if the source is under the control of the player, return the original damage
            // noteably, this should NOT include mind control targets
            if ((source->IsHunterPet() || source->IsPet() || source->IsSummon()) && source->IsControlledByPlayer())
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Source is a player-controlled pet or summon, returning original value of ({}).", amount);

                return amount;
            }

            //
            // Multiplier calculation
            //
            float damageMultiplier = 1.0f;

            // if the source is a player AND the target is that same player AND the value is damage (negative), use the map's multiplier
            if (source->GetTypeId() == TYPEID_PLAYER && source->GetGUID() == target->GetGUID() && amount < 0)
            {
                // if this aura damages based on a percent of the player's max health, use the un-level-scaled multiplier
                if (_isAuraWithEffectType(spellInfo, SPELL_AURA_PERIODIC_DAMAGE_PERCENT))
                {
                    damageMultiplier = sourceMapABInfo->worldDamageHealingMultiplier;
                    if (_debug_damage_and_healing)
                    {
                        LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Spell damage based on percent of max health. Ignore level scaling.");
                        LOG_DEBUG("module.AutoBalance_DamageHealingCC",
                                "AutoBalance_UnitScript::_Modify_Damage_Healing: Source is a player and the target is that same player, using the map's (level-scaling ignored) multiplier: ({})",
                                damageMultiplier
                        );
                    }
                }
                else
                {
                    damageMultiplier = sourceMapABInfo->scaledWorldDamageHealingMultiplier;
                    if (_debug_damage_and_healing)
                    {
                        LOG_DEBUG("module.AutoBalance_DamageHealingCC",
                                "AutoBalance_UnitScript::_Modify_Damage_Healing: Source is a player and the target is that same player, using the map's multiplier: ({})",
                                damageMultiplier
                        );
                    }
                }
            }
            // if the target is a player AND the value is healing (positive), use the map's damage multiplier
            // (player to player healing was already eliminated in the Source and Target Checking section)
            else if (target->GetTypeId() == TYPEID_PLAYER && amount >= 0)
            {
                damageMultiplier = targetMapABInfo->scaledWorldDamageHealingMultiplier;
                if (_debug_damage_and_healing)
                {
                    LOG_DEBUG("module.AutoBalance_DamageHealingCC",
                              "AutoBalance_UnitScript::_Modify_Damage_Healing: A non-player is healing a player, using the map's multiplier: ({})",
                              damageMultiplier
                    );
                }
            }
            // if the target is a player AND the source is not a creature, use the map's multiplier
            else if (target->GetTypeId() == TYPEID_PLAYER && source->GetTypeId() != TYPEID_UNIT && amount < 0)
            {
                // if this aura damages based on a percent of the player's max health, use the un-level-scaled multiplier
                if (_isAuraWithEffectType(spellInfo, SPELL_AURA_PERIODIC_DAMAGE_PERCENT))
                {
                    damageMultiplier = targetMapABInfo->worldDamageHealingMultiplier;
                    if (_debug_damage_and_healing)
                    {
                        LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Spell damage based on percent of max health. Ignore level scaling.");
                        LOG_DEBUG("module.AutoBalance_DamageHealingCC",
                                "AutoBalance_UnitScript::_Modify_Damage_Healing: Target is a player and the source is not a creature, using the map's (level-scaling-ignored) multiplier: ({})",
                                damageMultiplier
                        );
                    }
                }
                else
                {
                    damageMultiplier = targetMapABInfo->scaledWorldDamageHealingMultiplier;
                    if (_debug_damage_and_healing)
                    {
                        LOG_DEBUG("module.AutoBalance_DamageHealingCC",
                                "AutoBalance_UnitScript::_Modify_Damage_Healing: Target is a player and the source is not a creature, using the map's multiplier: ({})",
                                damageMultiplier
                        );
                    }
                }
            }
            // otherwise, use the source creature's damage multiplier
            else
            {
                // if this aura damages based on a percent of the player's max health, use the un-level-scaled multiplier
                if (_isAuraWithEffectType(spellInfo, SPELL_AURA_PERIODIC_DAMAGE_PERCENT))
                {
                    damageMultiplier = source->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo")->DamageMultiplier;
                    if (_debug_damage_and_healing)
                    {
                        LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Spell damage based on percent of max health. Ignore level scaling.");
                        LOG_DEBUG("module.AutoBalance_DamageHealingCC",
                                "AutoBalance_UnitScript::_Modify_Damage_Healing: Using the source creature's (level-scaling ignored) damage multiplier: ({})",
                                damageMultiplier
                        );
                    }
                }
                // non percent-based, used the normal multiplier
                else
                {
                    damageMultiplier = source->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo")->ScaledDamageMultiplier;
                    if (_debug_damage_and_healing)
                    {
                        LOG_DEBUG("module.AutoBalance_DamageHealingCC",
                                "AutoBalance_UnitScript::_Modify_Damage_Healing: Using the source creature's damage multiplier: ({})",
                                damageMultiplier
                        );
                    }
                }
            }

            // we are good to go, return the original damage times the multiplier
            if (_debug_damage_and_healing)
                LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Returning modified {}: ({}) * ({}) = ({})",
                    amount <= 0 ? "damage" : "healing",
                    amount,
                    damageMultiplier,
                    amount * damageMultiplier
                );

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

        bool _isAuraWithEffectType(SpellInfo const* spellInfo, AuraType auraType, bool log = false)
        {
            // if the spell is not defined, return false
            if (!spellInfo)
            {
                if (log) { LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_isAuraWithEffectType: SpellInfo is null, returning false."); }
                return false;
            }

            // if the spell doesn't have any effects, return false
            if (!spellInfo->GetEffects().size())
            {
                if (log) { LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_isAuraWithEffectType: SpellInfo has no effects, returning false."); }
                return false;
            }

            // iterate through the spell effects
            for (SpellEffectInfo effect : spellInfo->GetEffects())
            {
                // if the effect is not an aura, continue to next effect
                if (!effect.IsAura())
                {
                    if (log) { LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_isAuraWithEffectType: SpellInfo has an effect that is not an aura, continuing to next effect."); }
                    continue;
                }

                if (effect.ApplyAuraName == auraType)
                {
                    // if the effect is an aura of the target type, return true
                    LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_isAuraWithEffectType: SpellInfo has an aura of the target type, returning true.");
                    return true;
                }
            }

            // if no aura effect of type auraType was found, return false
            if (log) { LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_isAuraWithEffectType: SpellInfo has no aura of the target type, returning false."); }
            return false;
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
            amount = _Modify_GameObject_Damage_Healing(target, source, amount, spellInfo);

            if (_debug_damage_and_healing) _Debug_Output("OnGameObjectModifyHealth", target, source, amount, "AFTER:", spellInfo->SpellName[0], spellInfo->Id);
        }

    private:

        [[maybe_unused]] bool _debug_damage_and_healing = false; // defaults to false, overwritten in each function

        void _Debug_Output(std::string function_name, GameObject* target, Unit* source, int32 amount, std::string prefix = "", std::string spell_name = "Unknown Spell", uint32 spell_id = 0)
        {
            if (target && source && amount)
            {
                LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_GameObjectScript::{}: {} {} {} {} ({} - {})",
                    function_name,
                    prefix,
                    source->GetName(),
                    amount,
                    target->GetName(),
                    spell_name,
                    spell_id
                );
            }
            else if (target && source)
            {
                LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_GameObjectScript::{}: {} {} 0 {} ({} - {})",
                    function_name,
                    prefix,
                    source->GetName(),
                    target->GetName(),
                    spell_name,
                    spell_id
                );
            }
            else if (target && amount)
            {
                LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_GameObjectScript::{}: {} ?? {} {} ({} - {})",
                    function_name,
                    prefix,
                    amount,
                    target->GetName(),
                    spell_name,
                    spell_id
                );
            }
            else if (target)
            {
                LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_GameObjectScript::{}: {} ?? ?? {} ({} - {})",
                    function_name,
                    prefix,
                    target->GetName(),
                    spell_name,
                    spell_id
                );
            }
            else
            {
                LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_GameObjectScript::{}: {} W? T? F? ({} - {})",
                    function_name,
                    prefix,
                    spell_name,
                    spell_id
                );
            }
        }

        int32 _Modify_GameObject_Damage_Healing(GameObject* target, Unit* source, int32 amount, SpellInfo const* spellInfo)
        {
            //
            // Pre-flight Checks
            //

            // uncomment to debug this function
            bool _debug_damage_and_healing = (source && target && (source->GetTypeId() == TYPEID_PLAYER || source->IsControlledByPlayer()));

            // check that we're enabled globally, else return the original value
            if (!EnableGlobal)
            {
                if (_debug_damage_and_healing) LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_GameObjectScript::_Modify_GameObject_Damage_Healing: EnableGlobal is false, returning original value of ({}).", amount);

                return amount;
            }

            // make sure the target is in an instance, else return the original damage
            if (!(target->GetMap()->IsDungeon()))
            {
                if (_debug_damage_and_healing) LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_GameObjectScript::_Modify_GameObject_Damage_Healing: Target is not in an instance, returning original value of ({}).", amount);

                return amount;
            }

            // make sure the target is in the world, else return the original value
            if (!target->IsInWorld())
            {
                if (_debug_damage_and_healing) LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_GameObjectScript::_Modify_GameObject_Damage_Healing: Target does not exist in the world, returning original value of ({}).", amount);

                return amount;
            }

            // if the spell ID is in our "never modify" list, return the original value
            if
            (
                spellInfo &&
                spellInfo->Id &&
                std::find
                (
                    spellIdsToNeverModify.begin(),
                    spellIdsToNeverModify.end(),
                    spellInfo->Id
                ) != spellIdsToNeverModify.end()
            )
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_UnitScript::_Modify_Damage_Healing: Spell {}({}) is in the never modify list, returning original value of ({}).",
                        spellInfo->SpellName[0],
                        spellInfo->Id,
                        amount
                    );

                return amount;
            }

            // get the map's info
            AutoBalanceMapInfo *targetMapABInfo = target->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            // if the target's map is not enabled, return the original damage
            if (!targetMapABInfo->enabled)
            {
                if (_debug_damage_and_healing) LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_GameObjectScript::_Modify_GameObject_Damage_Healing: Target's map is not enabled, returning original value of ({}).", amount);

                return amount;
            }

            //
            // Multiplier calculation
            //

            // calculate the new damage amount using the map's World Health Multiplier
            int32 newAmount = _Calculate_Amount_For_GameObject(target, amount, targetMapABInfo->worldHealthMultiplier);

            if (_debug_damage_and_healing)
                LOG_DEBUG("module.AutoBalance_DamageHealingCC", "AutoBalance_GameObjectScript::_Modify_GameObject_Damage_Healing: Returning modified damage: ({}) -> ({})", amount, newAmount);

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
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnCreateMap(): Map {} ({}{})",
                map->GetMapName(),
                map->GetId(),
                map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
            );

            // clear out any previously-recorded data
            map->CustomData.Erase("AutoBalanceMapInfo");

            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            if (map->IsDungeon())
            {
                // get the map's LFG stats even if not enabled
                LFGDungeonEntry const* dungeon = GetLFGDungeon(map->GetId(), map->GetDifficulty());
                if (dungeon) {
                    mapABInfo->lfgMinLevel = dungeon->MinLevel;
                    mapABInfo->lfgMaxLevel = dungeon->MaxLevel;
                    mapABInfo->lfgTargetLevel = dungeon->TargetLevel;
                }
                // if this is a heroic dungeon that isn't in LFG, get the stats from the non-heroic version
                else if (map->IsHeroic())
                {
                    LFGDungeonEntry const* nonHeroicDungeon = nullptr;
                    if (map->GetDifficulty() == DUNGEON_DIFFICULTY_HEROIC)
                    {
                        nonHeroicDungeon = GetLFGDungeon(map->GetId(), DUNGEON_DIFFICULTY_NORMAL);
                    }
                    else if (map->GetDifficulty() == RAID_DIFFICULTY_10MAN_HEROIC)
                    {
                        nonHeroicDungeon = GetLFGDungeon(map->GetId(), RAID_DIFFICULTY_10MAN_NORMAL);
                    }
                    else if (map->GetDifficulty() == RAID_DIFFICULTY_25MAN_HEROIC)
                    {
                        nonHeroicDungeon = GetLFGDungeon(map->GetId(), RAID_DIFFICULTY_25MAN_NORMAL);
                    }

                    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnCreateMap(): Map {} ({}{}) | is a Heroic dungeon that is not in LFG. Using non-heroic LFG levels.",
                        map->GetMapName(),
                        map->GetId(),
                        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
                    );

                    if (nonHeroicDungeon)
                    {
                        mapABInfo->lfgMinLevel = nonHeroicDungeon->MinLevel;
                        mapABInfo->lfgMaxLevel = nonHeroicDungeon->MaxLevel;
                        mapABInfo->lfgTargetLevel = nonHeroicDungeon->TargetLevel;
                    }
                    else
                    {
                        LOG_ERROR("module.AutoBalance", "AutoBalance_AllMapScript::OnCreateMap(): Map {} ({}{}) | Could not determine LFG level ranges for this map. Level will bet set to 0.",
                            map->GetMapName(),
                            map->GetId(),
                            map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
                        );
                    }
                }

                if (map->GetInstanceId())
                {
                    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnCreateMap(): Map {} ({}{}) | is an instance of a map. Loading initial map data.",
                        map->GetMapName(),
                        map->GetId(),
                        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
                    );
                    UpdateMapDataIfNeeded(map);

                    // provide a concise summary of the map data we collected
                    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnCreateMap(): Map {} ({}{}) | LFG levels ({}-{}) (target {}). {} for AutoBalancing.",
                        map->GetMapName(),
                        map->GetId(),
                        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                        mapABInfo->lfgMinLevel ? std::to_string(mapABInfo->lfgMinLevel) : "?",
                        mapABInfo->lfgMaxLevel ? std::to_string(mapABInfo->lfgMaxLevel) : "?",
                        mapABInfo->lfgTargetLevel ? std::to_string(mapABInfo->lfgTargetLevel) : "?",
                        mapABInfo->enabled ? "Enabled" : "Disabled"
                    );
                }
                else
                {
                    LOG_DEBUG(
                        "module.AutoBalance", "AutoBalance_AllMapScript::OnCreateMap(): Map {} ({}) | is an instance base map.",
                        map->GetMapName(),
                        map->GetId()
                    );
                }
            }
        }

        // hook triggers after the player has already entered the world
        void OnPlayerEnterAll(Map* map, Player* player)
        {
            if (!EnableGlobal)
                return;

            if (!map->IsDungeon())
                return;

            LOG_DEBUG("module.AutoBalance", "AutoBalance:: {}", SPACER);

            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnPlayerEnterAll: Player {}{} | enters {} ({}{})",
                player->GetName(),
                player->IsGameMaster() ? " (GM)" : "",
                map->GetMapName(),
                map->GetId(),
                map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
            );

            // get the map's info
            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            // store the previous difficulty for comparison later
            int prevAdjustedPlayerCount = mapABInfo->adjustedPlayerCount;

            // add player to this map's player list
            AddPlayerToMap(map, player);

            // recalculate the zone's level stats
            mapABInfo->highestCreatureLevel = 0;
            mapABInfo->lowestCreatureLevel = 0;
            //mapABInfo->avgCreatureLevel = 0;
            mapABInfo->activeCreatureCount = 0;

            WorldSession* session = player->GetSession();
            LocaleConstant locale = session->GetSessionDbLocaleIndex();

            // if the previous player count is the same as the new player count, update without force
            if (prevAdjustedPlayerCount == mapABInfo->adjustedPlayerCount)
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnPlayerEnterAll: Player difficulty unchanged at {}. Updating map data (no force).",
                    mapABInfo->adjustedPlayerCount
                );

                // Update the map's data
                UpdateMapDataIfNeeded(map, false);
            }
            else
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnPlayerEnterAll: Player difficulty changed from ({})->({}). Updating map data (force).",
                    prevAdjustedPlayerCount,
                    mapABInfo->adjustedPlayerCount
                );

                // Update the map's data, forced
                UpdateMapDataIfNeeded(map, true);
            }

            // see which existing creatures are active
            for (std::vector<Creature*>::iterator creatureIterator = mapABInfo->allMapCreatures.begin(); creatureIterator != mapABInfo->allMapCreatures.end(); ++creatureIterator)
            {
                AddCreatureToMapCreatureList(*creatureIterator, false, true);
            }

            // Notify players of the change
            if (PlayerChangeNotify && mapABInfo->enabled)
            {
                if (map->GetEntry()->IsDungeon() && player)
                {
                    if (mapABInfo->playerCount)
                    {
                        for (std::vector<Player*>::const_iterator playerIterator = mapABInfo->allMapPlayers.begin(); playerIterator != mapABInfo->allMapPlayers.end(); ++playerIterator)
                        {
                            Player* thisPlayer = *playerIterator;
                            if (thisPlayer)
                            {
                                ChatHandler chatHandle = ChatHandler(thisPlayer->GetSession());
                                InstanceMap* instanceMap = map->ToInstanceMap();

                                std::string instanceDifficulty; if (instanceMap->IsHeroic()) instanceDifficulty = "Heroic"; else instanceDifficulty = "Normal";

                                if (thisPlayer && thisPlayer == player) // This is the player that entered
                                {
                                    chatHandle.PSendSysMessage(ABGetLocaleText(locale, "welcome_to_player").c_str(),
                                        map->GetMapName(),
                                        instanceMap->GetMaxPlayers(),
                                        instanceDifficulty.c_str(),
                                        mapABInfo->playerCount,
                                        mapABInfo->adjustedPlayerCount);

                                    // notify GMs that they won't be accounted for
                                    if (player->IsGameMaster())
                                    {
                                        chatHandle.PSendSysMessage(ABGetLocaleText(locale, "welcome_to_gm").c_str());
                                    }
                                }
                                else
                                {
                                    // announce non-GMs entering the instance only
                                    if (!player->IsGameMaster())
                                    {
                                        chatHandle.PSendSysMessage(ABGetLocaleText(locale, "announce_non_gm_entering_instance").c_str(), player->GetName().c_str(), mapABInfo->playerCount, mapABInfo->adjustedPlayerCount);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // hook triggers just before the player left the world
        void OnPlayerLeaveAll(Map* map, Player* player)
        {
            if (!EnableGlobal)
                return;

            if (!map->IsDungeon())
                return;

            LOG_DEBUG("module.AutoBalance", "AutoBalance:: {}", SPACER);

            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnPlayerLeaveAll: Player {}{} | exits {} ({}{})",
                player->GetName(),
                player->IsGameMaster() ? " (GM)" : "",
                map->GetMapName(),
                map->GetId(),
                map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
            );

            // get the map's info
            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            // store the previous difficulty for comparison later
            int prevAdjustedPlayerCount = mapABInfo->adjustedPlayerCount;

            // remove this player from this map's player list
            bool playerWasRemoved = RemovePlayerFromMap(map, player);

            // report the number of players in the map
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnPlayerLeaveAll: There are {} player(s) left in the map.", mapABInfo->allMapPlayers.size());

            // if a player was NOT removed, return now - stats don't need to be updated
            if (!playerWasRemoved)
            {
                return;
            }

            // recalculate the zone's level stats
            mapABInfo->highestCreatureLevel = 0;
            mapABInfo->lowestCreatureLevel = 0;
            //mapABInfo->avgCreatureLevel = 0;
            mapABInfo->activeCreatureCount = 0;

            // see which existing creatures are active
            for (std::vector<Creature*>::iterator creatureIterator = mapABInfo->allMapCreatures.begin(); creatureIterator != mapABInfo->allMapCreatures.end(); ++creatureIterator)
            {
                AddCreatureToMapCreatureList(*creatureIterator, false, true);
            }

            // if the previous player count is the same as the new player count, update without force
            if (prevAdjustedPlayerCount == mapABInfo->adjustedPlayerCount)
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnPlayerLeaveAll: Player difficulty unchanged at {}. Updating map data (no force).",
                    mapABInfo->adjustedPlayerCount
                );

                // Update the map's data
                UpdateMapDataIfNeeded(map, false);
            }
            else
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnPlayerLeaveAll: Player difficulty changed from ({})->({}). Updating map data (force).",
                    prevAdjustedPlayerCount,
                    mapABInfo->adjustedPlayerCount
                );

                // Update the map's data, forced
                UpdateMapDataIfNeeded(map, true);
            }

            // updates the player count and levels for the map
            if (map->GetEntry() && map->GetEntry()->IsDungeon())
            {
                {
                    mapABInfo->playerCount = mapABInfo->allMapPlayers.size();
                    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnPlayerLeaveAll: Player {} left the instance.",
                        player->GetName(),
                        mapABInfo->playerCount,
                        mapABInfo->adjustedPlayerCount
                    );
                }
            }

            // Notify remaining players in the instance that a player left
            if (PlayerChangeNotify && mapABInfo->enabled)
            {
                if (map->GetEntry()->IsDungeon() && player && !player->IsGameMaster())
                {
                    if (mapABInfo->playerCount)
                    {
                        for (std::vector<Player*>::const_iterator playerIterator = mapABInfo->allMapPlayers.begin(); playerIterator != mapABInfo->allMapPlayers.end(); ++playerIterator)
                        {
                            Player* thisPlayer = *playerIterator;
                            if (thisPlayer && thisPlayer != player)
                            {
                                ChatHandler chatHandle = ChatHandler(thisPlayer->GetSession());

                                if (mapABInfo->combatLocked)
                                {
                                    chatHandle.PSendSysMessage(ABGetLocaleText(thisPlayer->GetSession()->GetSessionDbLocaleIndex(), "leaving_instance_combat").c_str(),
                                        player->GetName().c_str(),
                                        mapABInfo->adjustedPlayerCount);
                                }
                                else
                                {
                                    chatHandle.PSendSysMessage(ABGetLocaleText(thisPlayer->GetSession()->GetSessionDbLocaleIndex(), "leaving_instance").c_str(),
                                        player->GetName().c_str(),
                                        mapABInfo->playerCount,
                                        mapABInfo->adjustedPlayerCount);
                                }
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

    void OnBeforeCreatureSelectLevel(const CreatureTemplate* /*creatureTemplate*/, Creature* creature, uint8 &level) override
    {
        Map* creatureMap = creature->GetMap();

        if (creatureMap && creatureMap->IsDungeon())
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance:: {}", SPACER);
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnBeforeCreatureSelectLevel: Creature {} ({}) | Entry ID: ({}) | Spawn ID: ({})",
                        creature->GetName(),
                        level,
                        creature->GetEntry(),
                        creature->GetSpawnId()
            );

            // Create the new creature's AB info
            AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

            // mark this creature as brand new so that only the level will be modified before creation
            creatureABInfo->isBrandNew = true;

            // if the creature already has a selectedLevel on it, we have already processed it and can re-use that value
            if (creatureABInfo->selectedLevel)
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnBeforeCreatureSelectLevel: Creature {} ({}) | has already been processed, using level {}.",
                            creature->GetName(),
                            creatureABInfo->UnmodifiedLevel,
                            creatureABInfo->selectedLevel
                );

                level = creatureABInfo->selectedLevel;
                return;
            }

            // Update the map's data if it is out of date (just before changing the map's creature list)
            UpdateMapDataIfNeeded(creature->GetMap());

            Map* creatureMap = creature->GetMap();
            InstanceMap* instanceMap = creatureMap->ToInstanceMap();

            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnBeforeCreatureSelectLevel: Creature {} ({}) | is in map {} ({}{}{}{})",
                        creature->GetName(),
                        level,
                        creatureMap->GetMapName(),
                        creatureMap->GetId(),
                        instanceMap ? "-" + std::to_string(instanceMap->GetInstanceId()) : "",
                        instanceMap ? ", " + std::to_string(instanceMap->GetMaxPlayers()) + "-player" : "",
                        instanceMap ? instanceMap->IsHeroic() ? " Heroic" : " Normal" : ""
            );

            // Set level originally intended for the creature
            creatureABInfo->UnmodifiedLevel = level;

            // add the creature to the map's tracking list
            AddCreatureToMapCreatureList(creature);

            // Update the map's data if it is out of date (just after changing the map's creature list)
            UpdateMapDataIfNeeded(creature->GetMap());

            // do an initial modification run of the creature, but don't update the level yet
            ModifyCreatureAttributes(creature);

            if (isCreatureRelevant(creature))
            {
            // set the new creature level
                level = creatureABInfo->selectedLevel;

                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnBeforeCreatureSelectLevel: Creature {} ({}) | will spawn in as level ({}).",
                            creature->GetName(),
                            creatureABInfo->UnmodifiedLevel,
                            creatureABInfo->selectedLevel
                );
            }
            else
            {
                // don't change level value
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnBeforeCreatureSelectLevel: Creature {} ({}) | will spawn in at its original level ({}).",
                            creature->GetName(),
                            creatureABInfo->UnmodifiedLevel,
                            creatureABInfo->selectedLevel
                );
            }
        }
    }

    void Creature_SelectLevel(const CreatureTemplate* /* cinfo */, Creature* creature) override
    {
        // ensure we're in a dungeon with a creature
        if (
            !creature ||
            !creature->GetMap() ||
            !creature->GetMap()->IsDungeon() ||
            !creature->GetMap()->GetInstanceId()
        )
        {
            return;
        }

        // get the creature's info
        AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

        // If the creature is brand new, it needs more processing
        if (creatureABInfo->isBrandNew)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance:: {}", SPACER);

            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::Creature_SelectLevel: Creature {} ({}) | Entry ID: ({}) | Spawn ID: ({})",
                        creature->GetName(),
                        creature->GetLevel(),
                        creature->GetEntry(),
                        creature->GetSpawnId()
            );

            if (creatureABInfo->isBrandNew)
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::Creature_SelectLevel: Creature {} ({}) | is no longer brand new.",
                            creature->GetName(),
                            creature->GetLevel()
                );
                creatureABInfo->isBrandNew = false;
            }

            // Update the map's data if it is out of date
            UpdateMapDataIfNeeded(creature->GetMap());

            ModifyCreatureAttributes(creature);

            // store the creature's max health value for validation in `OnCreatureAddWorld`
            creatureABInfo->initialMaxHealth = creature->GetMaxHealth();

            AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

            if (creature->GetLevel() != creatureABInfo->selectedLevel && isCreatureRelevant(creature))
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::Creature_SelectLevel: Creature {} ({}) | is set to level ({}).",
                            creature->GetName(),
                            creature->GetLevel(),
                            creatureABInfo->selectedLevel
                );
                creature->SetLevel(creatureABInfo->selectedLevel);
            }
        }
        else
        {
            LOG_ERROR("module.AutoBalance", "AutoBalance_AllCreatureScript::Creature_SelectLevel: Creature {} ({}) | is new to the instance but wasn't flagged as brand new. Please open an issue.",
                        creature->GetName(),
                        creature->GetLevel()
            );
        }
    }

    void OnCreatureAddWorld(Creature* creature) override
    {
        if (creature->GetMap()->IsDungeon())
        {
            Map* creatureMap = creature->GetMap();
            InstanceMap* instanceMap = creatureMap->ToInstanceMap();
            AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

            // final checks on the creature before spawning
            if (isCreatureRelevant(creature))
            {
                // level check
                if (creature->GetLevel() != creatureABInfo->selectedLevel && !creature->IsSummon())
                {
                    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnCreatureAddWorld: Creature {} ({}) | is set to level ({}) just after being added to the world.",
                                creature->GetName(),
                                creature->GetLevel(),
                                creatureABInfo->selectedLevel
                    );
                    creature->SetLevel(creatureABInfo->selectedLevel);
                }

                // max health check
                if (creature->GetMaxHealth() != creatureABInfo->initialMaxHealth)
                {

                    float oldMaxHealth = creature->GetMaxHealth();
                    float healthPct = creature->GetHealthPct();
                    creature->SetMaxHealth(creatureABInfo->initialMaxHealth);
                    creature->SetHealth(creature->GetMaxHealth() * (healthPct / 100));

                    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnCreatureAddWorld: Creature {} ({}) | had its max health changed from ({})->({}) just after being added to the world.",
                                creature->GetName(),
                                creature->GetLevel(),
                                oldMaxHealth,
                                creatureABInfo->initialMaxHealth
                    );
                }
            }

            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnCreatureAddWorld: Creature {} ({}) | added to map {} ({}{}{}{})",
                        creature->GetName(),
                        creature->GetLevel(),
                        creatureMap->GetMapName(),
                        creatureMap->GetId(),
                        instanceMap ? "-" + std::to_string(instanceMap->GetInstanceId()) : "",
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

            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnCreatureRemoveWorld: Creature {} ({}) | Entry ID: ({}) | Spawn ID: ({})",
                        creature->GetName(),
                        creature->GetLevel(),
                        creature->GetEntry(),
                        creature->GetSpawnId()

            );

            InstanceMap* instanceMap = creature->GetMap()->ToInstanceMap();
            Map* map = sMapMgr->FindBaseMap(creature->GetMapId());

            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnCreatureRemoveWorld: Creature {} ({}) | removed from map {} ({}{}{}{})",
                        creature->GetName(),
                        creature->GetLevel(),
                        map->GetMapName(),
                        map->GetId(),
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
        // ensure we're in a dungeon with a creature
        if (
            !creature ||
            !creature->GetMap() ||
            !creature->GetMap()->IsDungeon() ||
            !creature->GetMap()->GetInstanceId()
        )
        {
            return;
        }

        // update map data before making creature changes
        UpdateMapDataIfNeeded(creature->GetMap());

        // If the config is out of date and the creature was reset, run modify against it
        if (ResetCreatureIfNeeded(creature))
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance:: {}", SPACER);

            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnAllCreatureUpdate: Creature {} ({}) | Entry ID: ({}) | Spawn ID: ({})",
                        creature->GetName(),
                        creature->GetLevel(),
                        creature->GetEntry(),
                        creature->GetSpawnId()
            );

            // Update the map's data if it is out of date
            UpdateMapDataIfNeeded(creature->GetMap());

            ModifyCreatureAttributes(creature);

            AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

            if (creature->GetLevel() != creatureABInfo->selectedLevel && isCreatureRelevant(creature))
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnAllCreatureUpdate: Creature {} ({}) | is set to level ({}).",
                            creature->GetName(),
                            creature->GetLevel(),
                            creatureABInfo->selectedLevel
                );
                creature->SetLevel(creatureABInfo->selectedLevel);
            }
        }
    }

    // Reset the passed creature to stock if the config has changed
    bool ResetCreatureIfNeeded(Creature* creature)
    {
        // make sure we have a creature
        if (!creature || !isCreatureRelevant(creature))
        {
            return false;
        }

        // get (or create) map and creature info
        AutoBalanceMapInfo *mapABInfo=creature->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
        AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

        // if creature is dead and mapConfigTime is 0, skip for now
        if (creature->isDead() && creatureABInfo->mapConfigTime == 1)
        {
            return false;
        }
        // if the creature is dead but mapConfigTime is NOT 0, we set it to 0 so that it will be recalculated if revived
        // also remember that this creature was once alive but is now dead
        else if (creature->isDead())
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance:: {}", SPACER);
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ResetCreatureIfNeeded: Creature {} ({}) | is dead and mapConfigTime is not 0 - prime for reset if revived.", creature->GetName(), creature->GetLevel());
            creatureABInfo->mapConfigTime = 1;
            creatureABInfo->wasAliveNowDead = true;
            return false;
        }

        // if the config is outdated, reset the creature
        if (creatureABInfo->mapConfigTime < mapABInfo->mapConfigTime)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance:: {}", SPACER);

            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ResetCreatureIfNeeded: Creature {} ({}) | Entry ID: ({}) | Spawn ID: ({})",
                        creature->GetName(),
                        creature->GetLevel(),
                        creature->GetEntry(),
                        creature->GetSpawnId()
            );

            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ResetCreatureIfNeeded: Creature {} ({}) | Map config time is out of date ({} < {}). Resetting creature before modify.",
                        creature->GetName(),
                        creature->GetLevel(),
                        creatureABInfo->mapConfigTime,
                        mapABInfo->mapConfigTime
            );

            // retain some values
            uint8 unmodifiedLevel = creatureABInfo->UnmodifiedLevel;
            bool isActive = creatureABInfo->isActive;
            bool wasAliveNowDead = creatureABInfo->wasAliveNowDead;
            bool isInCreatureList = creatureABInfo->isInCreatureList;

            // reset AutoBalance modifiers
            creature->CustomData.Erase("AutoBalanceCreatureInfo");
            AutoBalanceCreatureInfo *creatureABInfo = creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

            // grab the creature's template and the original creature's stats
            CreatureTemplate const* creatureTemplate = creature->GetCreatureTemplate();

            // set the creature's level
            if (creature->GetLevel() != unmodifiedLevel)
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ResetCreatureIfNeeded: Creature {} ({}) | is set to level ({}).",
                            creature->GetName(),
                            creature->GetLevel(),
                            unmodifiedLevel
                );
                creature->SetLevel(unmodifiedLevel);
                creature->UpdateAllStats();
            }
            else
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ResetCreatureIfNeeded: Creature {} ({}) | is already set to level ({}).",
                            creature->GetName(),
                            creature->GetLevel(),
                            unmodifiedLevel
                );
            }

            // get the creature's base stats
            CreatureBaseStats const* origCreatureBaseStats = sObjectMgr->GetCreatureBaseStats(unmodifiedLevel, creatureTemplate->unit_class);

            // health
            float currentHealthPercent = (float)creature->GetHealth() / (float)creature->GetMaxHealth();
            creature->SetMaxHealth(origCreatureBaseStats->GenerateHealth(creatureTemplate));
            creature->SetHealth((float)origCreatureBaseStats->GenerateHealth(creatureTemplate) * currentHealthPercent);

            // mana
            if (creature->getPowerType() == POWER_MANA && creature->GetPower(POWER_MANA) >= 0 && creature->GetMaxPower(POWER_MANA) > 0)
            {
                float currentManaPercent = creature->GetPower(POWER_MANA) / creature->GetMaxPower(POWER_MANA);
                creature->SetMaxPower(POWER_MANA, origCreatureBaseStats->GenerateMana(creatureTemplate));
                creature->SetPower(POWER_MANA, creature->GetMaxPower(POWER_MANA) * currentManaPercent);
            }

            // armor
            creature->SetArmor(origCreatureBaseStats->GenerateArmor(creatureTemplate));

            // restore the saved data
            creatureABInfo->UnmodifiedLevel = unmodifiedLevel;
            creatureABInfo->isActive = isActive;
            creatureABInfo->wasAliveNowDead = wasAliveNowDead;
            creatureABInfo->isInCreatureList = isInCreatureList;

            // damage and ccduration are handled using AutoBalanceCreatureInfo data only

            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ResetCreatureIfNeeded: Creature {} ({}) is reset to its original stats.",
                        creature->GetName(),
                        creature->GetLevel()
            );

            // return true to indicate that the creature was reset
            return true;
        }

        // creature was not reset, return false
        return false;

    }

    void ModifyCreatureAttributes(Creature* creature)
    {
        // make sure we have a creature
        if (!creature)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: creature is null.");
            return;
        }

        // grab creature and map data
        AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");
        Map* map = creature->GetMap();
        InstanceMap* instanceMap = map->ToInstanceMap();
        AutoBalanceMapInfo *mapABInfo=instanceMap->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

        // mark the creature as updated using the current settings if needed
        // if this creature is brand new, do not update this so that it will be re-processed next OnCreatureUpdate
        if (creatureABInfo->mapConfigTime < mapABInfo->mapConfigTime && !creatureABInfo->isBrandNew)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | Map config time set to ({}).",
                        creature->GetName(),
                        creature->GetLevel(),
                        mapABInfo->mapConfigTime
            );
            creatureABInfo->mapConfigTime = mapABInfo->mapConfigTime;
        }

        // check to make sure that the creature's map is enabled for scaling
        if (!mapABInfo->enabled)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | is in map {} ({}{}{}{}) that is not enabled, not changed.",
                creature->GetName(),
                creatureABInfo->UnmodifiedLevel,
                map->GetMapName(),
                map->GetId(),
                instanceMap ? "-" + std::to_string(instanceMap->GetInstanceId()) : "",
                instanceMap ? ", " + std::to_string(instanceMap->GetMaxPlayers()) + "-player" : "",
                instanceMap ? instanceMap->IsHeroic() ? " Heroic" : " Normal" : ""
            );

            // return the creature back to their original level, if it's not already
            creatureABInfo->selectedLevel = creatureABInfo->UnmodifiedLevel;

            return;
        }

        // if the creature isn't relevant, don't modify it
        if (!isCreatureRelevant(creature))
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | is not relevant, not changed.",
                creature->GetName(),
                creatureABInfo->UnmodifiedLevel
            );

            // return the creature back to their original level, if it's not already
            creatureABInfo->selectedLevel = creatureABInfo->UnmodifiedLevel;

            return;
        }

        // if this creature is below 85% of the minimum LFG level for the map, make no changes
        // if this creature is above 115% of the maximum LFG level for the map, make no changes
        // if this is a critter that is substantial enough to be considered a real enemy, still modify it
        // if this is a trigger, still modify it
        if (
            (
                (creatureABInfo->UnmodifiedLevel < (uint8)(((float)mapABInfo->lfgMinLevel * .85f) + 0.5f)) ||
                (creatureABInfo->UnmodifiedLevel > (uint8)(((float)mapABInfo->lfgMaxLevel * 1.15f) + 0.5f))
            ) &&
            (
                !(creature->IsCritter() && creatureABInfo->UnmodifiedLevel >= 5 && creature->GetMaxHealth() > 100) &&
                !creature->IsTrigger()
            )
        )
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | is a {} outside of the expected NPC level range for the map ({} to {}), not modified.",
                        creature->GetName(),
                        creatureABInfo->UnmodifiedLevel,
                        creature->IsCritter() ? "critter" : "creature",
                        (uint8)(((float)mapABInfo->lfgMinLevel * .85f) + 0.5f),
                        (uint8)(((float)mapABInfo->lfgMaxLevel * 1.15f) + 0.5f)
            );

            creatureABInfo->selectedLevel = creatureABInfo->UnmodifiedLevel;

            return;
        }

        // if the creature was dead (but this function is being called because they are being revived), reset it and allow modifications
        if (creatureABInfo->wasAliveNowDead)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | was dead but appears to be alive now, reset wasAliveNowDead flag.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            // if the creature was dead, reset it
            creatureABInfo->wasAliveNowDead = false;
        }
        // if the creature is dead and wasn't marked as dead by this script, simply skip
        else if (creature->isDead())
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | is dead, do not modify.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            return;
        }

        CreatureTemplate const *creatureTemplate = creature->GetCreatureTemplate();

        // check to see if the creature is in the forced num players list
        uint32 forcedNumPlayers = GetForcedNumPlayers(creatureTemplate->Entry);

        if (forcedNumPlayers == 0)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | is in the forced num players list with a value of 0, not changed.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            return; // forcedNumPlayers 0 means that the creature is contained in DisabledID -> no scaling
        }

        // start with the map's adjusted player count
        uint32 adjustedPlayerCount = mapABInfo->adjustedPlayerCount;

        // if the forced value is set and the adjusted player count is above the forced value, change it to match
        if (forcedNumPlayers > 0 && adjustedPlayerCount > forcedNumPlayers)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | is in the forced num players list with a value of {}, adjusting adjustedPlayerCount to match.", creature->GetName(), creatureABInfo->UnmodifiedLevel, forcedNumPlayers);
            adjustedPlayerCount = forcedNumPlayers;
        }

        // store the current player count in the creature and map's data
        creatureABInfo->instancePlayerCount = adjustedPlayerCount;

        if (!creatureABInfo->instancePlayerCount) // no players in map, do not modify attributes
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | is on a map with no players, not changed.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            return;
        }

        if (!sABScriptMgr->OnBeforeModifyAttributes(creature, creatureABInfo->instancePlayerCount))
            return;

        // only scale levels if level scaling is enabled and the instance's average creature level is not within the skip range
        if
        (
            LevelScaling &&
            (
                (mapABInfo->avgCreatureLevel > mapABInfo->highestPlayerLevel + mapABInfo->levelScalingSkipHigherLevels || mapABInfo->levelScalingSkipHigherLevels == 0) ||
                (mapABInfo->avgCreatureLevel < mapABInfo->highestPlayerLevel - mapABInfo->levelScalingSkipLowerLevels || mapABInfo->levelScalingSkipLowerLevels == 0)
            ) &&
            !creatureABInfo->neverLevelScale
        )
        {
            uint8 selectedLevel;

            // handle "special" creatures
            // note that these already passed a more complex check above
            if (
                (creature->IsTotem() && creature->IsSummon() && creatureABInfo->summoner && creatureABInfo->summoner->IsPlayer()) ||
                (
                    creature->IsCritter() && creatureABInfo->UnmodifiedLevel <= 5 && creature->GetMaxHealth() <= 100
                )
            )
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | is a {} that will not be level scaled, but will have modifiers set.",
                            creature->GetName(),
                            creatureABInfo->UnmodifiedLevel,
                            creature->IsTotem() ? "totem" : "critter"
                );

                selectedLevel = creatureABInfo->UnmodifiedLevel;
            }
            // if we're using dynamic scaling, calculate the creature's level based relative to the highest player level in the map
            else if (LevelScalingMethod == AUTOBALANCE_SCALING_DYNAMIC)
            {
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

                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaled to level ({}) via dynamic scaling.",
                            creature->GetName(),
                            creatureABInfo->UnmodifiedLevel,
                            selectedLevel
                );
            }
            // otherwise we're using "fixed" scaling and should use the highest player level in the map
            else
            {
                selectedLevel = mapABInfo->highestPlayerLevel;
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaled to level ({}) via fixed scaling.", creature->GetName(), creatureABInfo->UnmodifiedLevel, selectedLevel);
            }

            creatureABInfo->selectedLevel = selectedLevel;

            if (creature->GetLevel() != selectedLevel)
            {
                if (!creatureABInfo->isBrandNew)
                {
                    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | is set to new selectedLevel ({}).",
                                creature->GetName(),
                                creatureABInfo->UnmodifiedLevel,
                                selectedLevel
                    );

                    creature->SetLevel(selectedLevel);
                }
            }
        }
        else if (!LevelScaling)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | not level scaled due to level scaling being disabled.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            creatureABInfo->selectedLevel = creatureABInfo->UnmodifiedLevel;
        }
        else if (creatureABInfo->neverLevelScale)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | not level scaled due to being marked as multipliers only.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            creatureABInfo->selectedLevel = creatureABInfo->UnmodifiedLevel;
        }
        else
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | not level scaled due the instance's average creature level being inside the skip range.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            creatureABInfo->selectedLevel = creatureABInfo->UnmodifiedLevel;
        }

        if (creatureABInfo->isBrandNew)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | is brand new, do not modify level or stats yet.",
                        creature->GetName(),
                        creatureABInfo->UnmodifiedLevel
            );

            return;
        }

        CreatureBaseStats const* origCreatureBaseStats = sObjectMgr->GetCreatureBaseStats(creatureABInfo->UnmodifiedLevel, creatureTemplate->unit_class);
        CreatureBaseStats const* newCreatureBaseStats = sObjectMgr->GetCreatureBaseStats(creatureABInfo->selectedLevel, creatureTemplate->unit_class);

        // Inflection Point
        AutoBalanceInflectionPointSettings inflectionPointSettings = getInflectionPointSettings(instanceMap, isBossOrBossSummon(creature));

        // Generate the default multiplier
        float defaultMultiplier = getDefaultMultiplier(instanceMap, inflectionPointSettings);

        if (!sABScriptMgr->OnAfterDefaultMultiplier(creature, defaultMultiplier))
            return;

        // Stat Modifiers
        AutoBalanceStatModifiers statModifiers = getStatModifiers(map, creature);
        float statMod_global        = statModifiers.global;
        float statMod_health        = statModifiers.health;
        float statMod_mana          = statModifiers.mana;
        float statMod_armor         = statModifiers.armor;
        float statMod_damage        = statModifiers.damage;
        float statMod_ccDuration    = statModifiers.ccduration;

        // Storage for the final values applied to the creature
        uint32 newFinalHealth = 0;
        uint32 newFinalMana = 0;
        uint32 newFinalArmor = 0;

        //
        //  Health Scaling
        //
        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | ---------- HEALTH MULTIPLIER ----------",
                    creature->GetName(),
                    creatureABInfo->selectedLevel
        );

        float healthMultiplier = defaultMultiplier * statMod_global * statMod_health;
        float scaledHealthMultiplier;

        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | HealthMultiplier: ({}) = defaultMultiplier ({}) * statMod_global ({}) * statMod_health ({})",
                    creature->GetName(),
                    creatureABInfo->selectedLevel,
                    healthMultiplier,
                    defaultMultiplier,
                    statMod_global,
                    statMod_health
        );

        // Can't be less than MinHPModifier
        if (healthMultiplier <= MinHPModifier)
        {
            healthMultiplier = MinHPModifier;

            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | HealthMultiplier: ({}) - capped to MinHPModifier ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        healthMultiplier,
                        MinHPModifier
            );
        }

        // set the non-level-scaled health multiplier on the creature's AB info
        creatureABInfo->HealthMultiplier = healthMultiplier;

        // only level scale health if level scaling is enabled and the creature level has been altered
        if (LevelScaling && creatureABInfo->selectedLevel != creatureABInfo->UnmodifiedLevel)
        {
            // the max health that the creature had before we did anything with it
            float origHealth = origCreatureBaseStats->GenerateHealth(creatureTemplate);
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | origHealth ({}) = origCreatureBaseStats->GenerateHealth(creatureTemplate)",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        origHealth
            );

            // the base health of the new creature level for this creature's class
            // uses a custom smoothing formula to smooth transitions between expansions
            float newBaseHealth = getBaseExpansionValueForLevel(newCreatureBaseStats->BaseHealth, mapABInfo->highestPlayerLevel);
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newBaseHealth ({}) = getBaseExpansionValueForLevel(newCreatureBaseStats->BaseHealth, mapABInfo->highestPlayerLevel ({}))",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        newBaseHealth,
                        mapABInfo->highestPlayerLevel
            );

            // the health of the creature at its new level (before per-player scaling)
            float newHealth = newBaseHealth * creatureTemplate->ModHealth;
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newHealth ({}) = newBaseHealth ({}) * creature ModHealth ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        newHealth,
                        newBaseHealth,
                        creatureTemplate->ModHealth
            );

            // the multiplier that would need to be applied to the creature's original health to get the new level's health (before per-player scaling)
            float newHealthMultiplier = newHealth / origHealth;
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newHealthMultiplier ({}) = newHealth ({}) / origHealth ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        newHealthMultiplier,
                        newHealth,
                        origHealth
            );

            // the multiplier that would need to be applied to the creature's original health to get the new level's health (after per-player scaling)
            scaledHealthMultiplier = healthMultiplier * newHealthMultiplier;
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaledHealthMultiplier ({}) = healthMultiplier ({}) * newHealthMultiplier ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        scaledHealthMultiplier,
                        healthMultiplier,
                        newHealthMultiplier
            );

            // the actual health value to be applied to the level-scaled and player-scaled creature
            newFinalHealth = round(origHealth * scaledHealthMultiplier);
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newFinalHealth ({}) = origHealth ({}) * scaledHealthMultiplier ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        newFinalHealth,
                        origHealth,
                        scaledHealthMultiplier
            );
        }
        else
        {
            // the non-level-scaled health multiplier is the same as the level-scaled health multiplier
            scaledHealthMultiplier = healthMultiplier;
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaledHealthMultiplier ({}) = healthMultiplier ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        scaledHealthMultiplier,
                        healthMultiplier
            );

            // the original health of the creature
            uint32 origHealth = origCreatureBaseStats->GenerateHealth(creatureTemplate);
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | origHealth ({}) = origCreatureBaseStats->GenerateHealth(creatureTemplate)",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        origHealth
            );

            // the actual health value to be applied to the player-scaled creature
            newFinalHealth = round(origHealth * creatureABInfo->HealthMultiplier);
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newFinalHealth ({}) = origHealth ({}) * HealthMultiplier ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        newFinalHealth,
                        origHealth,
                        creatureABInfo->HealthMultiplier
            );
        }

        //
        //  Mana Scaling
        //
        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | ---------- MANA MULTIPLIER ----------",
                    creature->GetName(),
                    creatureABInfo->selectedLevel
        );

        float manaMultiplier = defaultMultiplier * statMod_global * statMod_mana;
        float scaledManaMultiplier;

        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | ManaMultiplier: ({}) = defaultMultiplier ({}) * statMod_global ({}) * statMod_mana ({})",
                    creature->GetName(),
                    creatureABInfo->selectedLevel,
                    manaMultiplier,
                    defaultMultiplier,
                    statMod_global,
                    statMod_mana
        );

        // Can't be less than MinManaModifier
        if (manaMultiplier <= MinManaModifier)
        {
            manaMultiplier = MinManaModifier;

            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | ManaMultiplier: ({}) - capped to MinManaModifier ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        manaMultiplier,
                        MinManaModifier
            );
        }

        // if the creature doesn't have mana, set the multiplier to 0.0
        if (!origCreatureBaseStats->GenerateMana(creatureTemplate))
        {
            manaMultiplier = 0.0f;
            creatureABInfo->ManaMultiplier = 0.0f;
            scaledManaMultiplier = 0.0f;

            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | Creature doesn't have mana, multiplier set to ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        creatureABInfo->ManaMultiplier
            );
        }
        // if the creature has mana, continue calculations
        else
        {
            // set the non-level-scaled mana multiplier on the creature's AB info
            creatureABInfo->ManaMultiplier = manaMultiplier;
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | ManaMultiplier: ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        creatureABInfo->ManaMultiplier
            );

            // only level scale mana if level scaling is enabled and the creature level has been altered
            if (LevelScaling && creatureABInfo->selectedLevel != creatureABInfo->UnmodifiedLevel)
            {
                // the max mana that the creature had before we did anything with it
                uint32 origMana = origCreatureBaseStats->GenerateMana(creatureTemplate);
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | origMana ({}) = origCreatureBaseStats->GenerateMana(creatureTemplate)",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            origMana
                );

                // the max mana that the creature would have at its new level
                // there is no per-expansion adjustment for mana
                uint32 newMana = newCreatureBaseStats->GenerateMana(creatureTemplate);
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newMana ({}) = newCreatureBaseStats->GenerateMana(creatureTemplate)",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            newMana
                );

                // the multiplier that would need to be applied to the creature's original mana to get the new level's mana (before per-player scaling)
                float newManaMultiplier = (float)newMana / (float)origMana;
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newManaMultiplier ({}) = newMana ({}) / origMana ({})",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            newManaMultiplier,
                            newMana,
                            origMana
                );

                // the multiplier that would need to be applied to the creature's original mana to get the new level's mana (after per-player scaling)
                scaledManaMultiplier = manaMultiplier * newManaMultiplier;
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaledManaMultiplier ({}) = manaMultiplier ({}) * newManaMultiplier ({})",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            scaledManaMultiplier,
                            manaMultiplier,
                            newManaMultiplier
                );

                // the actual mana value to be applied to the level-scaled and player-scaled creature
                newFinalMana = round(origMana * scaledManaMultiplier);
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newFinalMana ({}) = origMana ({}) * scaledManaMultiplier ({})",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            newFinalMana,
                            origMana,
                            scaledManaMultiplier
                );
            }
            else
            {
                // scaled mana multiplier is the same as the non-level-scaled mana multiplier
                scaledManaMultiplier = manaMultiplier;
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaledManaMultiplier ({}) = manaMultiplier ({})",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            scaledManaMultiplier,
                            manaMultiplier
                );

                // the original mana of the creature
                uint32 origMana = origCreatureBaseStats->GenerateMana(creatureTemplate);
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | origMana ({}) = origCreatureBaseStats->GenerateMana(creatureTemplate)",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            origMana
                );

                // the actual mana value to be applied to the player-scaled creature
                newFinalMana = round(origMana * creatureABInfo->ManaMultiplier);
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newFinalMana ({}) = origMana ({}) * creatureABInfo->ManaMultiplier ({})",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            newFinalMana,
                            origMana,
                            creatureABInfo->ManaMultiplier
                );
            }
        }

        //
        //  Armor Scaling
        //
        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | ---------- ARMOR MULTIPLIER ----------",
                    creature->GetName(),
                    creatureABInfo->selectedLevel
        );

        float armorMultiplier = defaultMultiplier * statMod_global * statMod_armor;
        float scaledArmorMultiplier;

        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | armorMultiplier: ({}) = defaultMultiplier ({}) * statMod_global ({}) * statMod_armor ({})",
                    creature->GetName(),
                    creatureABInfo->selectedLevel,
                    armorMultiplier,
                    defaultMultiplier,
                    statMod_global,
                    statMod_armor
        );

        // set the non-level-scaled armor multiplier on the creature's AB info
        creatureABInfo->ArmorMultiplier = armorMultiplier;

        // only level scale armor if level scaling is enabled and the creature level has been altered
        if (LevelScaling && creatureABInfo->selectedLevel != creatureABInfo->UnmodifiedLevel)
        {
            // the armor that the creature had before we did anything with it
            uint32 origArmor = origCreatureBaseStats->GenerateArmor(creatureTemplate);
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | origArmor ({}) = origCreatureBaseStats->GenerateArmor(creatureTemplate)",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        origArmor
            );

            // the armor that the creature would have at its new level
            // there is no per-expansion adjustment for armor
            uint32 newArmor = newCreatureBaseStats->GenerateArmor(creatureTemplate);
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newArmor ({}) = newCreatureBaseStats->GenerateArmor(creatureTemplate)",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        newArmor
            );

            // the multiplier that would need to be applied to the creature's original armor to get the new level's armor (before per-player scaling)
            float newArmorMultiplier = (float)newArmor / (float)origArmor;
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newArmorMultiplier ({}) = newArmor ({}) / origArmor ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        newArmorMultiplier,
                        newArmor,
                        origArmor
            );

            // the multiplier that would need to be applied to the creature's original armor to get the new level's armor (after per-player scaling)
            scaledArmorMultiplier = armorMultiplier * newArmorMultiplier;
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaledArmorMultiplier ({}) = armorMultiplier ({}) * newArmorMultiplier ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        scaledArmorMultiplier,
                        armorMultiplier,
                        newArmorMultiplier
            );

            // the actual armor value to be applied to the level-scaled and player-scaled creature
            newFinalArmor = round(origArmor * scaledArmorMultiplier);
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newFinalArmor ({}) = origArmor ({}) * scaledArmorMultiplier ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        newFinalArmor,
                        origArmor,
                        scaledArmorMultiplier
            );
        }
        else
        {
            // Scaled armor multiplier is the same as the non-level-scaled armor multiplier
            scaledArmorMultiplier = armorMultiplier;
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaledArmorMultiplier ({}) = armorMultiplier ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        scaledArmorMultiplier,
                        armorMultiplier
            );

            // the original armor of the creature
            uint32 origArmor = origCreatureBaseStats->GenerateArmor(creatureTemplate);
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | origArmor ({}) = origCreatureBaseStats->GenerateArmor(creatureTemplate)",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        origArmor
            );

            // the actual armor value to be applied to the player-scaled creature
            newFinalArmor = round(origArmor * creatureABInfo->ArmorMultiplier);
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newFinalArmor ({}) = origArmor ({}) * creatureABInfo->ArmorMultiplier ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        newFinalArmor,
                        origArmor,
                        creatureABInfo->ArmorMultiplier
            );
        }

        //
        //  Damage Scaling
        //
        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | ---------- DAMAGE MULTIPLIER ----------",
                    creature->GetName(),
                    creatureABInfo->selectedLevel
        );

        float damageMultiplier = defaultMultiplier * statMod_global * statMod_damage;
        float scaledDamageMultiplier;

        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | DamageMultiplier: ({}) = defaultMultiplier ({}) * statMod_global ({}) * statMod_damage ({})",
                    creature->GetName(),
                    creatureABInfo->selectedLevel,
                    damageMultiplier,
                    defaultMultiplier,
                    statMod_global,
                    statMod_damage
        );

        // Can't be less than MinDamageModifier
        if (damageMultiplier <= MinDamageModifier)
        {
            damageMultiplier = MinDamageModifier;

            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | DamageMultiplier: ({}) - capped to MinDamageModifier ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        damageMultiplier,
                        MinDamageModifier
            );
        }

        // set the non-level-scaled damage multiplier on the creature's AB info
        creatureABInfo->DamageMultiplier = damageMultiplier;
        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | DamageMultiplier: ({})",
                    creature->GetName(),
                    creatureABInfo->selectedLevel,
                    creatureABInfo->DamageMultiplier
        );

        // only level scale damage if level scaling is enabled and the creature level has been altered
        if (LevelScaling && creatureABInfo->selectedLevel != creatureABInfo->UnmodifiedLevel)
        {

            // the original base damage of the creature
            // note that we don't mess with the damage modifier here since it applied equally to the original and new levels
            float origBaseDamage = origCreatureBaseStats->GenerateBaseDamage(creatureTemplate);
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | origBaseDamage ({}) = origCreatureBaseStats->GenerateBaseDamage(creatureTemplate)",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        origBaseDamage
            );

            // the base damage of the new creature level for this creature's class
            // uses a custom smoothing formula to smooth transitions between expansions
            float newBaseDamage = getBaseExpansionValueForLevel(newCreatureBaseStats->BaseDamage, mapABInfo->highestPlayerLevel);
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newBaseDamage ({}) = getBaseExpansionValueForLevel(newCreatureBaseStats->BaseDamage, mapABInfo->highestPlayerLevel ({}))",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        newBaseDamage,
                        mapABInfo->highestPlayerLevel
            );

            // the multiplier that would need to be applied to the creature's original damage to get the new level's damage (before per-player scaling)
            float newDamageMultiplier = newBaseDamage / origBaseDamage;
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | newDamageMultiplier ({}) = newBaseDamage ({}) / origBaseDamage ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        newDamageMultiplier,
                        newBaseDamage,
                        origBaseDamage
            );

            // the actual multiplier that will be used to scale the creature's damage (after per-player scaling)
            scaledDamageMultiplier = damageMultiplier * newDamageMultiplier;
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaledDamageMultiplier ({}) = damageMultiplier ({}) * newDamageMultiplier ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        scaledDamageMultiplier,
                        damageMultiplier,
                        newDamageMultiplier
            );
        }
        else
        {
            // the scaled damage multiplier is the same as the non-level-scaled damage multiplier
            scaledDamageMultiplier = damageMultiplier;
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaledDamageMultiplier ({}) = damageMultiplier ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        scaledDamageMultiplier,
                        origCreatureBaseStats->GenerateBaseDamage(creatureTemplate),
                        damageMultiplier
            );

        }

        //
        // Crowd Control Debuff Duration Scaling
        //

        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | ---------- CC DURATION MULTIPLIER ----------",
                    creature->GetName(),
                    creatureABInfo->selectedLevel
        );

        float ccDurationMultiplier;

        if (statMod_ccDuration != -1.0f)
        {
            // calculate CC Duration from the default multiplier and the config settings
            ccDurationMultiplier = defaultMultiplier * statMod_ccDuration;

            // Min/Max checking
            if (ccDurationMultiplier < MinCCDurationModifier)
            {
                ccDurationMultiplier = MinCCDurationModifier;
            }
            else if (ccDurationMultiplier > MaxCCDurationModifier)
            {
                ccDurationMultiplier = MaxCCDurationModifier;
            }

            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | ccDurationMultiplier: ({})",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        ccDurationMultiplier
            );
        }
        else
        {
            // the CC Duration will not be changed
            ccDurationMultiplier = 1.0f;
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | Crowd Control Duration will not be changed.",
                        creature->GetName(),
                        creatureABInfo->selectedLevel
            );
        }

        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | ccDurationMultiplier: ({}) = defaultMultiplier ({}) * statMod_ccDuration ({})",
                    creature->GetName(),
                    creatureABInfo->selectedLevel,
                    ccDurationMultiplier,
                    defaultMultiplier,
                    statMod_ccDuration
        );

        //
        //  Apply New Values
        //
        if (!sABScriptMgr->OnBeforeUpdateStats(creature, newFinalHealth, newFinalMana, damageMultiplier, newFinalArmor))
            return;

        uint32 prevMaxHealth = creature->GetMaxHealth();
        uint32 prevMaxPower = creature->GetMaxPower(Powers::POWER_MANA);
        uint32 prevHealth = creature->GetHealth();
        uint32 prevPower = creature->GetPower(Powers::POWER_MANA);

        uint32 prevPlayerDamageRequired = creature->GetPlayerDamageReq();
        uint32 prevCreateHealth = creature->GetCreateHealth();

        Powers pType = creature->getPowerType();

        creature->SetArmor(newFinalArmor);
        creature->SetModifierValue(UNIT_MOD_ARMOR, BASE_VALUE, (float)newFinalArmor);
        creature->SetCreateHealth(newFinalHealth);
        creature->SetMaxHealth(newFinalHealth);
        creature->ResetPlayerDamageReq();
        creature->SetCreateMana(newFinalMana);
        creature->SetMaxPower(Powers::POWER_MANA, newFinalMana);
        creature->SetModifierValue(UNIT_MOD_ENERGY, BASE_VALUE, (float)100.0f);
        creature->SetModifierValue(UNIT_MOD_RAGE, BASE_VALUE, (float)100.0f);
        creature->SetModifierValue(UNIT_MOD_HEALTH, BASE_VALUE, (float)newFinalHealth);
        creature->SetModifierValue(UNIT_MOD_MANA, BASE_VALUE, (float)newFinalMana);
        creatureABInfo->ScaledHealthMultiplier = scaledHealthMultiplier;
        creatureABInfo->ScaledManaMultiplier = scaledManaMultiplier;
        creatureABInfo->ScaledArmorMultiplier = scaledArmorMultiplier;
        creatureABInfo->ScaledDamageMultiplier = scaledDamageMultiplier;
        creatureABInfo->CCDurationMultiplier = ccDurationMultiplier;

        // adjust the current health as appropriate
        uint32 scaledCurHealth = 0;
        uint32 scaledCurPower = 0;

        // if this is a summon and it's a clone of its summoner, keep the health and mana values of the summon
        // only do this once, when `_isSummonCloneOfSummoner(creature)` returns true but !creatureABInfo->isCloneOfSummoner is false
        if
        (
            creature->IsSummon() &&
            _isSummonCloneOfSummoner(creature) &&
            !creatureABInfo->isCloneOfSummoner
        )
        {
            creatureABInfo->isCloneOfSummoner = true;
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | Summon is a clone of its summoner, keeping health and mana values.",
                        creature->GetName(),
                        creatureABInfo->selectedLevel
            );

            if (prevHealth && prevMaxHealth)
            {
                scaledCurHealth = prevHealth;
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaledCurHealth ({}) = prevHealth ({})",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            scaledCurHealth,
                            prevHealth
                );
            }

            if (prevPower && prevMaxPower)
            {
                scaledCurPower = prevPower;
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaledCurPower ({}) = prevPower ({})",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            scaledCurPower,
                            prevPower
                );
            }
        }
        else
        {
            if (prevHealth && prevMaxHealth)
            {
                scaledCurHealth = float(newFinalHealth) / float(prevMaxHealth) * float(prevHealth);
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaledCurHealth ({}) = float(newFinalHealth) ({}) / float(prevMaxHealth) ({}) * float(prevHealth) ({})",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            scaledCurHealth,
                            newFinalHealth,
                            prevMaxHealth,
                            prevHealth
                );
            }
            else
            {
                scaledCurHealth = 0;
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaledCurHealth ({}) = 0",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            scaledCurHealth
                );
            }

            if (prevPower && prevMaxPower)
            {
                scaledCurPower = float(newFinalMana) / float(prevMaxPower) * float(prevPower);
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaledCurPower ({}) = float(newFinalMana) ({}) / float(prevMaxPower) ({}) * float(prevPower) ({})",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            scaledCurPower,
                            newFinalMana,
                            prevMaxPower,
                            prevPower
                );
            }
            else
            {
                scaledCurPower = 0;
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | scaledCurPower ({}) = 0",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            scaledCurPower
                );
            }
        }

        creature->SetHealth(scaledCurHealth);
        if (pType == Powers::POWER_MANA)
            creature->SetPower(Powers::POWER_MANA, scaledCurPower);
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
            uint32 scaledPlayerDmgReq = float(prevPlayerDamageRequired) * float(newFinalHealth) / float(prevCreateHealth);
            // Do some math
            creature->LowerPlayerDamageReq(playerDamageRequired - scaledPlayerDmgReq, true);
        }

        //
        // Reward Scaling
        //

        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | ---------- REWARD SCALING ----------",
                    creature->GetName(),
                    creatureABInfo->selectedLevel
        );

        // calculate the average multiplier after level scaling is applied
        float avgHealthDamageMultipliers;

        // only if one of the scaling options is enabled
        if (RewardScalingXP || RewardScalingMoney)
        {
            // use health and damage to calculate the average multiplier
            avgHealthDamageMultipliers = (scaledHealthMultiplier + scaledDamageMultiplier) / 2.0f;
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | avgHealthDamageMultipliers ({}) = (scaledHealthMultiplier ({}) + scaledDamageMultiplier ({})) / 2.0f",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        avgHealthDamageMultipliers,
                        scaledHealthMultiplier,
                        scaledDamageMultiplier
            );
        }
        else
        {
            // Reward scaling is disabled
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | Reward scaling is disabled.",
                        creature->GetName(),
                        creatureABInfo->selectedLevel
            );
        }

        // XP Scaling
        if (RewardScalingXP)
        {
            if (RewardScalingMethod == AUTOBALANCE_SCALING_FIXED)
            {
                creatureABInfo->XPModifier = RewardScalingXPModifier;
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | Fixed Mode: XPModifier ({}) = RewardScalingXPModifier ({})",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            creatureABInfo->XPModifier,
                            RewardScalingXPModifier
                );
            }
            else if (RewardScalingMethod == AUTOBALANCE_SCALING_DYNAMIC)
            {
                creatureABInfo->XPModifier = avgHealthDamageMultipliers * RewardScalingXPModifier;
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | Dynamic Mode: XPModifier ({}) = avgHealthDamageMultipliers ({}) * RewardScalingXPModifier ({})",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            creatureABInfo->XPModifier,
                            avgHealthDamageMultipliers,
                            RewardScalingXPModifier
                );
            }
        }

        // Money Scaling
        if (RewardScalingMoney)
        {

            if (RewardScalingMethod == AUTOBALANCE_SCALING_FIXED)
            {
                creatureABInfo->MoneyModifier = RewardScalingMoneyModifier;
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | Fixed Mode: MoneyModifier ({}) = RewardScalingMoneyModifier ({})",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            creatureABInfo->MoneyModifier,
                            RewardScalingMoneyModifier
                );
            }
            else if (RewardScalingMethod == AUTOBALANCE_SCALING_DYNAMIC)
            {
                creatureABInfo->MoneyModifier = avgHealthDamageMultipliers * RewardScalingMoneyModifier;
                LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | Dynamic Mode: MoneyModifier ({}) = avgHealthDamageMultipliers ({}) * RewardScalingMoneyModifier ({})",
                            creature->GetName(),
                            creatureABInfo->selectedLevel,
                            creatureABInfo->MoneyModifier,
                            avgHealthDamageMultipliers,
                            RewardScalingMoneyModifier
                );
            }
        }

        // update all stats
        creature->UpdateAllStats();

        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | ---------- FINAL STATS ----------",
                    creature->GetName(),
                    creatureABInfo->selectedLevel
        );

        LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | Health ({}/{} {:.1f}%) -> ({}/{} {:.1f}%)",
                    creature->GetName(),
                    creatureABInfo->selectedLevel,
                    prevHealth,
                    prevMaxHealth,
                    prevMaxHealth ? float(prevHealth) / float(prevMaxHealth) * 100.0f : 0.0f,
                    creature->GetHealth(),
                    creature->GetMaxHealth(),
                    creature->GetMaxHealth() ? float(creature->GetHealth()) / float(creature->GetMaxHealth()) * 100.0f : 0.0f
        );

        if (prevPower && prevMaxPower && pType == Powers::POWER_MANA)
        {
            LOG_DEBUG("module.AutoBalance_StatGeneration", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | Mana ({}/{} {:.1f}%) -> ({}/{} {:.1f}%)",
                        creature->GetName(),
                        creatureABInfo->selectedLevel,
                        prevPower,
                        prevMaxPower,
                        prevMaxPower ? float(prevPower) / float(prevMaxPower) * 100.0f : 0.0f,
                        creature->GetPower(Powers::POWER_MANA),
                        creature->GetMaxPower(Powers::POWER_MANA),
                        creature->GetMaxPower(Powers::POWER_MANA) ? float(creature->GetPower(Powers::POWER_MANA)) / float(creature->GetMaxPower(Powers::POWER_MANA)) * 100.0f : 0.0f
            );
        }

        // debug log the new stat multipliers stored in CreatureABInfo in a compact, single-line format
        if (creatureABInfo->UnmodifiedLevel != creatureABInfo->selectedLevel)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}->{}) | Multipliers: H:{:.3f}->{:.3f} M:{:.3f}->{:.3f} A:{:.3f}->{:.3f} D:{:.3f}->{:.3f} CC:{:.3f} XP:{:.3f} $:{:.3f}",
                    creature->GetName(),
                    creatureABInfo->UnmodifiedLevel,
                    creatureABInfo->selectedLevel,
                    creatureABInfo->HealthMultiplier,
                    creatureABInfo->ScaledHealthMultiplier,
                    creatureABInfo->ManaMultiplier,
                    creatureABInfo->ScaledManaMultiplier,
                    creatureABInfo->ArmorMultiplier,
                    creatureABInfo->ScaledArmorMultiplier,
                    creatureABInfo->DamageMultiplier,
                    creatureABInfo->ScaledDamageMultiplier,
                    creatureABInfo->CCDurationMultiplier,
                    creatureABInfo->XPModifier,
                    creatureABInfo->MoneyModifier
            );
        }
        else
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) | Multipliers: H:{:.3f} M:{:.3f} A:{:.3f} D:{:.3f} CC:{:.3f} XP:{:.3f} $:{:.3f}",
                    creature->GetName(),
                    creatureABInfo->UnmodifiedLevel,
                    creatureABInfo->HealthMultiplier,
                    creatureABInfo->ManaMultiplier,
                    creatureABInfo->ArmorMultiplier,
                    creatureABInfo->DamageMultiplier,
                    creatureABInfo->CCDurationMultiplier,
                    creatureABInfo->XPModifier,
                    creatureABInfo->MoneyModifier
            );
        }


    }

private:
    bool _isSummonCloneOfSummoner(Creature* summon)
    {
        // if the summon doesn't exist or isn't a summon
        if (!summon || !summon->IsSummon())
        {
            return false;
        }

        // get the summon's info
        AutoBalanceCreatureInfo* summonABInfo = summon->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

        // get the saved summoner
        Creature* summoner = summonABInfo->summoner;

        // if the summoner doesn't exist
        if (!summoner)
        {
            return false;
        }

        // if this creature's ID is in the list of creatures that are not clones of their summoner (creatureIDsThatAreNotClones), return false
        if (
            std::find
            (
                creatureIDsThatAreNotClones.begin(),
                creatureIDsThatAreNotClones.end(),
                summon->GetEntry()
            ) != creatureIDsThatAreNotClones.end()
        )
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::_isSummonCloneOfSummoner: Creature {} ({}) | creatureIDsThatAreNotClones contains this creature's ID ({}) | false",
                        summon->GetName(),
                        summonABInfo->selectedLevel,
                        summon->GetEntry()
            );
            return false;
        }


        // create a running score for this check
        int8 score = 0;

        LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::_isSummonCloneOfSummoner: Creature {} ({}) | Is this a clone of it's summoner {} ({})?",
                    summon->GetName(),
                    summonABInfo->selectedLevel,
                    summoner->GetName(),
                    summoner->GetLevel()
        );


        // if the entry ID is the same, +2
        if (summon->GetEntry() == summoner->GetEntry())
        {
            score += 2;
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::_isSummonCloneOfSummoner: Creature {} ({}) | Entry: ({}) == ({}) | score: +2 = ({})",
                        summon->GetName(),
                        summonABInfo->selectedLevel,
                        summon->GetEntry(),
                        summoner->GetEntry(),
                        score
            );
        }

        // if the max health is the same, +3
        if (summon->GetMaxHealth() == summoner->GetMaxHealth())
        {
            score += 3;
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::_isSummonCloneOfSummoner: Creature {} ({}) | MaxHealth: ({}) == ({}) | score: +3 = ({})",
                        summon->GetName(),
                        summonABInfo->selectedLevel,
                        summon->GetMaxHealth(),
                        summoner->GetMaxHealth(),
                        score
            );
        }

        // if the type (humanoid, dragonkin, etc) is the same, +1
        if (summon->GetCreatureType() == summoner->GetCreatureType())
        {
            score += 1;
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::_isSummonCloneOfSummoner: Creature {} ({}) | CreatureType: ({}) == ({}) | score: +1 = ({})",
                        summon->GetName(),
                        summonABInfo->selectedLevel,
                        summon->GetCreatureType(),
                        summoner->GetCreatureType(),
                        score
            );
        }

        // if the name is the same, +2
        if (summon->GetName() == summoner->GetName())
        {
            score += 2;
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::_isSummonCloneOfSummoner: Creature {} ({}) | Name: ({}) == ({}) | score: +2 = ({})",
                        summon->GetName(),
                        summonABInfo->selectedLevel,
                        summon->GetName(),
                        summoner->GetName(),
                        score
            );
        }
        // if the summoner's name is a part of the summon's name, +1
        else if (summon->GetName().find(summoner->GetName()) != std::string::npos)
        {
            score += 1;
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::_isSummonCloneOfSummoner: Creature {} ({}) | Name: ({}) contains ({}) | score: +1 = ({})",
                        summon->GetName(),
                        summonABInfo->selectedLevel,
                        summon->GetName(),
                        summoner->GetName(),
                        score
            );
        }

        // if the display ID is the same, +1
        if (summon->GetDisplayId() == summoner->GetDisplayId())
        {
            score += 1;
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::_isSummonCloneOfSummoner: Creature {} ({}) | DisplayId: ({}) == ({}) | score: +1 = ({})",
                        summon->GetName(),
                        summonABInfo->selectedLevel,
                        summon->GetDisplayId(),
                        summoner->GetDisplayId(),
                        score
            );
        }

        // if the score is at least 5, consider this a clone
        if (score >= 5)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::_isSummonCloneOfSummoner: Creature {} ({}) | score ({}) >= 5 | true",
                        summon->GetName(),
                        summonABInfo->selectedLevel,
                        score
            );
            return true;
        }
        else
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::_isSummonCloneOfSummoner: Creature {} ({}) | score ({}) < 5 | false",
                        summon->GetName(),
                        summonABInfo->selectedLevel,
                        score
            );
            return false;
        }
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
            { "setoffset",     HandleABSetOffsetCommand,      SEC_GAMEMASTER,  Console::Yes },
            { "getoffset",     HandleABGetOffsetCommand,      SEC_PLAYER,      Console::Yes },
            { "mapstat",       HandleABMapStatsCommand,       SEC_PLAYER,      Console::Yes },
            { "creaturestat",  HandleABCreatureStatsCommand,  SEC_PLAYER,      Console::Yes }
        };

        static std::vector<ChatCommand> commandTable =
        {
            { "autobalance",  ABCommandTable },
            { "ab",           ABCommandTable },
        };
        return commandTable;
    }

    static bool HandleABSetOffsetCommand(ChatHandler* handler, const char* args)
    {
        if (!*args)
        {
            handler->PSendSysMessage(".autobalance setoffset #");
            handler->PSendSysMessage(ABGetLocaleText(handler->GetSession()->GetSessionDbLocaleIndex(), "set_offset_command_description").c_str());
            return false;
        }
        char* offset = strtok((char*)args, " ");
        int32 offseti = -1;

        if (offset)
        {
            offseti = (uint32)atoi(offset);
            std::vector<std::string> args = { std::to_string(offseti) };
            handler->PSendSysMessage(ABGetLocaleText(handler->GetSession()->GetSessionDbLocaleIndex(), "set_offset_command_success").c_str(), offseti);
            PlayerCountDifficultyOffset = offseti;
            globalConfigTime = GetCurrentConfigTime();
            return true;
        }
        else
            handler->PSendSysMessage(ABGetLocaleText(handler->GetSession()->GetSessionDbLocaleIndex(), "set_offset_command_error").c_str());
        return false;
    }

    static bool HandleABGetOffsetCommand(ChatHandler* handler, const char* /*args*/)
    {
        handler->PSendSysMessage(ABGetLocaleText(handler->GetSession()->GetSessionDbLocaleIndex(), "get_offset_command_success").c_str(), PlayerCountDifficultyOffset);
        return true;
    }

    static bool HandleABMapStatsCommand(ChatHandler* handler, const char* /*args*/)
    {
        Player *player = handler->GetPlayer();
        auto locale = handler->GetSession()->GetSessionDbLocaleIndex();

        AutoBalanceMapInfo *mapABInfo=player->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

        if (player->GetMap()->IsDungeon())
        {
            handler->PSendSysMessage("---");
            // Map basics
            handler->PSendSysMessage("%s (%u-player %s) | ID %u-%u%s",
                                    player->GetMap()->GetMapName(),
                                    player->GetMap()->ToInstanceMap()->GetMaxPlayers(),
                                    player->GetMap()->ToInstanceMap()->IsHeroic() ? "Heroic" : "Normal",
                                    player->GetMapId(),
                                    player->GetInstanceId(),
                                    mapABInfo->enabled ? "" : " | AutoBalance DISABLED");

            // if the map isn't enabled, don't display anything else
            // if (!mapABInfo->enabled) { return true; }

            // Player stats
            handler->PSendSysMessage("Players on map: %u (Lvl %u - %u)",
                                    mapABInfo->playerCount,
                                    mapABInfo->lowestPlayerLevel,
                                    mapABInfo->highestPlayerLevel
                                    );

            // Adjusted player count (multiple scenarios)
            if (mapABInfo->combatLockTripped)
            {
                handler->PSendSysMessage(ABGetLocaleText(locale, "adjusted_player_count_combat_locked").c_str(), mapABInfo->adjustedPlayerCount);
            }
            else if (mapABInfo->playerCount < mapABInfo->minPlayers && !PlayerCountDifficultyOffset)
            {
                handler->PSendSysMessage(ABGetLocaleText(locale, "adjusted_player_count_map_minimum").c_str(), mapABInfo->adjustedPlayerCount);
            }
            else if (mapABInfo->playerCount < mapABInfo->minPlayers && PlayerCountDifficultyOffset)
            {
                handler->PSendSysMessage(ABGetLocaleText(locale, "adjusted_player_count_map_minimum_difficulty_offset").c_str(), mapABInfo->adjustedPlayerCount, PlayerCountDifficultyOffset);
            }
            else if (PlayerCountDifficultyOffset)
            {
                handler->PSendSysMessage(ABGetLocaleText(locale, "adjusted_player_count_difficulty_offset").c_str(), mapABInfo->adjustedPlayerCount, PlayerCountDifficultyOffset);
            }
            else
            {
                handler->PSendSysMessage(ABGetLocaleText(locale, "adjusted_player_count").c_str(), mapABInfo->adjustedPlayerCount);
            }

            // LFG levels
            handler->PSendSysMessage(ABGetLocaleText(locale, "lfg_range").c_str(), mapABInfo->lfgMinLevel, mapABInfo->lfgMaxLevel, mapABInfo->lfgTargetLevel);

            // Calculated map level (creature average)
            handler->PSendSysMessage(ABGetLocaleText(locale, "map_level").c_str(),
                                    (uint8)(mapABInfo->avgCreatureLevel+0.5f),
                                    mapABInfo->isLevelScalingEnabled && mapABInfo->enabled ? "->" + std::to_string(mapABInfo->highestPlayerLevel) + ABGetLocaleText(locale, "level_scaling_enabled").c_str() : ABGetLocaleText(locale, "level_scaling_disabled").c_str()
                                    );

            // World Health Multiplier
            handler->PSendSysMessage(ABGetLocaleText(locale, "world_health_multiplier").c_str(), mapABInfo->worldHealthMultiplier);

            // World Damage and Healing Multiplier
            if (mapABInfo->worldDamageHealingMultiplier != mapABInfo->scaledWorldDamageHealingMultiplier)
            {
                handler->PSendSysMessage(ABGetLocaleText(locale, "world_hostile_damage_healing_multiplier_to").c_str(),
                        mapABInfo->worldDamageHealingMultiplier,
                        mapABInfo->scaledWorldDamageHealingMultiplier
                        );
            }
            else
            {
                handler->PSendSysMessage(ABGetLocaleText(locale, "world_hostile_damage_healing_multiplier").c_str(),
                        mapABInfo->worldDamageHealingMultiplier
                        );
            }

            // Creature Stats
            handler->PSendSysMessage(ABGetLocaleText(locale, "original_creature_level_range").c_str(),
                                    mapABInfo->lowestCreatureLevel,
                                    mapABInfo->highestCreatureLevel,
                                    mapABInfo->avgCreatureLevel
                                    );
            handler->PSendSysMessage(ABGetLocaleText(locale, "active_total_creatures_in_map").c_str(),
                                    mapABInfo->activeCreatureCount,
                                    mapABInfo->allMapCreatures.size()
                                    );

            return true;
        }
        else
        {
            handler->PSendSysMessage(ABGetLocaleText(locale, "ab_command_only_in_instance").c_str());
            return false;
        }
    }

    static bool HandleABCreatureStatsCommand(ChatHandler* handler, const char* /*args*/)
    {
        Creature* target = handler->getSelectedCreature();

        auto locale = handler->GetSession()->GetSessionDbLocaleIndex();

        if (!target)
        {
            handler->SendSysMessage(LANG_SELECT_CREATURE);
            handler->SetSentErrorMessage(true);
            return false;
        }
        else if (!target->GetMap()->IsDungeon())
        {
            handler->PSendSysMessage(ABGetLocaleText(locale, "target_no_in_instance").c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        AutoBalanceCreatureInfo *targetABInfo=target->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

        handler->PSendSysMessage("---");
        handler->PSendSysMessage("%s (%u%s%s), %s",
                                  target->GetName(),
                                  targetABInfo->UnmodifiedLevel,
                                  isCreatureRelevant(target) && targetABInfo->UnmodifiedLevel != target->GetLevel() ? "->" + std::to_string(targetABInfo->selectedLevel) : "",
                                  isBossOrBossSummon(target) ? " | Boss" : "",
                                  targetABInfo->isActive ? ABGetLocaleText(locale, "active_for_map_stats").c_str() : ABGetLocaleText(locale, "ignored_for_map_stats").c_str());
        handler->PSendSysMessage(ABGetLocaleText(locale, "creature_difficulty_level").c_str(), targetABInfo->instancePlayerCount);

        // summon
        if (target->IsSummon() && targetABInfo->summoner && targetABInfo->isCloneOfSummoner)
        {
            handler->PSendSysMessage(ABGetLocaleText(locale, "clone_of_summon").c_str(), targetABInfo->summonerName, targetABInfo->summonerLevel);
        }
        else if (target->IsSummon() && targetABInfo->summoner)
        {
            handler->PSendSysMessage(ABGetLocaleText(locale, "summon_of_summon").c_str(), targetABInfo->summonerName, targetABInfo->summonerLevel);
        }
        else if (target->IsSummon())
        {
            handler->PSendSysMessage(ABGetLocaleText(locale, "summon_without_summoner").c_str());
        }

        // level scaled
        if (targetABInfo->UnmodifiedLevel != target->GetLevel())
        {
            handler->PSendSysMessage(ABGetLocaleText(locale, "health_multiplier_to").c_str(), targetABInfo->HealthMultiplier, targetABInfo->ScaledHealthMultiplier);
            handler->PSendSysMessage(ABGetLocaleText(locale, "mana_multiplier_to").c_str(), targetABInfo->ManaMultiplier, targetABInfo->ScaledManaMultiplier);
            handler->PSendSysMessage(ABGetLocaleText(locale, "armor_multiplier_to").c_str(), targetABInfo->ArmorMultiplier, targetABInfo->ScaledArmorMultiplier);
            handler->PSendSysMessage(ABGetLocaleText(locale, "damage_multiplier_to").c_str(), targetABInfo->DamageMultiplier, targetABInfo->ScaledDamageMultiplier);
        }
        // not level scaled
        else
        {
            handler->PSendSysMessage(ABGetLocaleText(locale, "health_multiplier").c_str(), targetABInfo->HealthMultiplier);
            handler->PSendSysMessage(ABGetLocaleText(locale, "mana_multiplier").c_str(), targetABInfo->ManaMultiplier);
            handler->PSendSysMessage(ABGetLocaleText(locale, "armor_multiplier").c_str(), targetABInfo->ArmorMultiplier);
            handler->PSendSysMessage(ABGetLocaleText(locale, "damage_multiplier").c_str(), targetABInfo->DamageMultiplier);
        }
        handler->PSendSysMessage(ABGetLocaleText(locale, "cc_duration_multiplier").c_str(), targetABInfo->CCDurationMultiplier);
        handler->PSendSysMessage(ABGetLocaleText(locale, "xp_money_multiplier").c_str(), targetABInfo->XPModifier, targetABInfo->MoneyModifier);

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

        AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

        if (mapABInfo->adjustedPlayerCount < MinPlayerReward)
            return;

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
            if (!itr->GetSource() || itr->GetSource()->IsGameMaster() || itr->GetSource()->GetLevel() < DEFAULT_MAX_LEVEL)
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
