/*
* Copyright (C) 2018 AzerothCore <http://www.azerothcore.org>
* Copyright (C) 2012 CVMagic <http://www.trinitycore.org/f/topic/6551-autobalance/>
* Copyright (C) 2008-2010 TrinityCore <http://www.trinitycore.org/>
* Copyright (C) 2006-2009 ScriptDev2 <https://scriptdev2.svn.sourceforge.net/>
* Copyright (C) 1985-2010 {} KalCorp  <http://server.dyndns.org/>
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
#include "MapManager.h"
#include "World.h"
#include "Map.h"
#include "ScriptMgr.h"
#include "Language.h"
#include <vector>
#include "AutoBalance.h"
#include "ScriptMgrMacros.h"
#include "Group.h"

#define BOOL_TO_STRING(b) ((b)? "true":"false")

bool ABScriptMgr::OnBeforeModifyAttributes(Creature *creature, uint32 & playerCount)
{
    bool ret=true;

    FOR_SCRIPTS_RET(ABModuleScript, itr, end, ret) // return true by default if not scripts

    if (!itr->second->OnBeforeModifyAttributes(creature, playerCount))
        ret=false; // we change ret value only when scripts return false

    return ret;
}

bool ABScriptMgr::OnAfterDefaultMultiplier(Creature *creature, float & defaultMultiplier)
{
    bool ret=true;

    FOR_SCRIPTS_RET(ABModuleScript, itr, end, ret) // return true by default if not scripts

    if (!itr->second->OnAfterDefaultMultiplier(creature, defaultMultiplier))
        ret=false; // we change ret value only when scripts return false

    return ret;
}

bool ABScriptMgr::OnBeforeUpdateStats(Creature* creature, uint32& scaledHealth, uint32& scaledMana, float& damageMultiplier, uint32& newBaseArmor)
{
    bool ret=true;

    FOR_SCRIPTS_RET(ABModuleScript, itr, end, ret)

    if (!itr->second->OnBeforeUpdateStats(creature, scaledHealth, scaledMana, damageMultiplier, newBaseArmor))
        ret=false;

    return ret;
}

ABModuleScript::ABModuleScript(const char* name) : ModuleScript(name)
{
    ScriptRegistry<ABModuleScript>::AddScript(this);
}

struct AutoBalanceCreatureInfo
{
    uint32 playerCount;
    float DamageMultiplier;
};

struct AutoBalanceMapInfo
{
    uint8 mapLevel = 0;
};

static std::map<uint32, AutoBalanceCreatureInfo> CreatureInfo; // A hook should be added to remove the mapped entry when the creature is dead or this should be added into the creature object
static std::map<uint32, AutoBalanceMapInfo> MapInfo;
static std::map<int, int> forcedCreatureIds;                   // The map values correspond with the AutoBalance.XX.Name entries in the configuration file.

// cheaphack for difficulty server-wide.
// Another value TODO in player class for the party leader's value to determine dungeon difficulty.
static int8 PlayerCountDifficultyOffset, LevelScaling, higherOffset, lowerOffset, numPlayerConf;
static uint32 rewardRaid, rewardDungeon, MinPlayerReward;
static bool enabled, LevelEndGameBoost, DungeonsOnly, PlayerChangeNotify, LevelUseDb, rewardEnabled, balanceInstance;
static float globalRate, healthMultiplier, manaMultiplier, armorMultiplier, damageMultiplier, MinHPModifier, MinDamageModifier, InflectionPoint;

int GetValidDebugLevel()
{
    int debugLevel = sConfigMgr->GetIntDefault("AutoBalance.DebugLevel", 2);

    if ((debugLevel < 0) || (debugLevel > 3))
    {
        return 1;
    }
    return debugLevel;
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

int GetForcedCreatureId(int creatureId)
{
    if(forcedCreatureIds.find(creatureId) == forcedCreatureIds.end()) // Don't want the forcedCreatureIds map to blowup to a massive empty array
    {
        return 0;
    }
    return forcedCreatureIds[creatureId];
}

class AutoBalance_WorldScript : public WorldScript
{
    public:
    AutoBalance_WorldScript() : WorldScript("AutoBalance_WorldScript") {}

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        SetInitialWorldSettings();
    }
    void OnStartup() override
    {}

    void SetInitialWorldSettings()
    {
        forcedCreatureIds.clear();

        LoadForcedCreatureIdsFromString(sConfigMgr->GetStringDefault("AutoBalance.ForcedID40", ""), 40);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetStringDefault("AutoBalance.ForcedID25", ""), 25);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetStringDefault("AutoBalance.ForcedID10", ""), 10);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetStringDefault("AutoBalance.ForcedID5", ""), 5);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetStringDefault("AutoBalance.ForcedID2", ""), 2);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetStringDefault("AutoBalance.DisabledID", ""), 0);

        //sLog->outInfo("----------------------------------------------------");
        //sLog->outInfo("  Powered by AutoBalance");
        //sLog->outInfo("----------------------------------------------------");
        //sLog->outInfo("  xPlayer = %4.1f ", sConfigMgr->GetFloatConfig(Config_xPlayer));
        //sLog->outInfo("  AutoInstance = %u ", sConfigMgr->GetIntDefault("AutoBalance.Instances", balanceInstance);
        //sLog->outInfo("  PlayerChangeNotify = %u ", sConfigMgr->GetIntDefault(PlayerChangeNotify));
        //sLog->outInfo("  Min.D.Mod = %4.2f ", sConfigMgr->GetFloatDefault("AutoBalance.MinDamageModifier", MinDamageModifier);
        //sLog->outInfo("  Min.HP.Mod = %4.2f ", sConfigMgr->GetFloatDefault("AutoBalance.MinHPModifier", MinHPModifier);
        //sLog->outInfo("  Debug   =  %u ", GetValidDebugLevel());
        //sLog->outInfo("----------------------------------------------------\n");

        enabled = sConfigMgr->GetIntDefault("AutoBalance.enable", 1) == 1;
        LevelEndGameBoost = sConfigMgr->GetIntDefault("AutoBalance.LevelEndGameBoost", 1) == 1;
        balanceInstance = sConfigMgr->GetIntDefault("AutoBalance.Instances", 1) == 1;
        DungeonsOnly = sConfigMgr->GetIntDefault("AutoBalance.DungeonsOnly", 1) == 1;
        PlayerChangeNotify = sConfigMgr->GetIntDefault("AutoBalance.PlayerChangeNotify", 1) == 1;
        LevelUseDb = sConfigMgr->GetIntDefault("AutoBalance.levelUseDbValuesWhenExists", 1) == 1;
        rewardEnabled = sConfigMgr->GetIntDefault("AutoBalance.reward.enable", 1) == 1;

        LevelScaling = sConfigMgr->GetIntDefault("AutoBalance.levelScaling", 1);
        PlayerCountDifficultyOffset = sConfigMgr->GetIntDefault("AutoBalance.playerCountDifficultyOffset", 0);
        higherOffset = sConfigMgr->GetIntDefault("AutoBalance.levelHigherOffset", 3);
        lowerOffset = sConfigMgr->GetIntDefault("AutoBalance.levelLowerOffset", 0);
        rewardRaid = sConfigMgr->GetIntDefault("AutoBalance.reward.raidToken", 49426);
        rewardDungeon = sConfigMgr->GetIntDefault("AutoBalance.reward.dungeonToken", 47241);
        MinPlayerReward = sConfigMgr->GetFloatDefault("AutoBalance.reward.MinPlayerReward", 1);

        InflectionPoint = sConfigMgr->GetFloatDefault("AutoBalance.InflectionPoint", 0.5f);
        globalRate = sConfigMgr->GetFloatDefault("AutoBalance.rate.global", 1.0f);
        healthMultiplier = sConfigMgr->GetFloatDefault("AutoBalance.rate.health", 1.0f);
        manaMultiplier = sConfigMgr->GetFloatDefault("AutoBalance.rate.mana", 1.0f);
        armorMultiplier = sConfigMgr->GetFloatDefault("AutoBalance.rate.armor", 1.0f);
        damageMultiplier = sConfigMgr->GetFloatDefault("AutoBalance.rate.damage", 1.0f);
        numPlayerConf=sConfigMgr->GetFloatDefault("AutoBalance.numPlayer", 1.0f);
        MinHPModifier = sConfigMgr->GetFloatDefault("AutoBalance.MinHPModifier", 0.1f);
        MinDamageModifier = sConfigMgr->GetFloatDefault("AutoBalance.MinDamageModifier", 0.1f);
    }
};

class AutoBalance_PlayerScript : public PlayerScript
{
    public:
        AutoBalance_PlayerScript() : PlayerScript("AutoBalance_PlayerScript") {}

        void OnLogin(Player *Player)
        {
            if (sConfigMgr->GetBoolDefault("AutoBalanceAnnounce.enable", true))
            {
                ChatHandler(Player->GetSession()).SendSysMessage("This server is running the |cff4CFF00AutoBalance |rmodule.");
            }
        }

        virtual void OnLevelChanged(Player* player, uint8 /*oldlevel*/)
        {
            if (!enabled || !player)
                return;

            if (LevelScaling == 0)
                return;

            AutoBalanceMapInfo *MapInfo = player->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            if (MapInfo->mapLevel < player->getLevel())
                MapInfo->mapLevel = player->getLevel();
        }
};

class AutoBalance_UnitScript : public UnitScript
{
    public:
    AutoBalance_UnitScript() : UnitScript("AutoBalance_UnitScript", true) {}

    uint32 DealDamage(Unit* AttackerUnit, Unit *playerVictim, uint32 damage, DamageEffectType damagetype)
    {
        if (AttackerUnit->GetMap()->IsDungeon() && playerVictim->GetMap()->IsDungeon())
            if (AttackerUnit->GetTypeId() != TYPEID_PLAYER)
            {
                //if (GetValidDebugLevel() >= 3)
                    //sLog->outInfo("### AutoBalance_UnitScript - Unit_DealDamage Attacker=%s Victim=%s Start Damage=%u",AttackerUnit->GetName(),playerVictim->GetName(),damage);
                damage = Modifer_DealDamage(AttackerUnit,damage);
                //if (GetValidDebugLevel() >= 3)
                    //sLog->outInfo("### AutoBalance_UnitScript - Unit_DealDamage Attacker=%s Victim=%s End Damage=%u",AttackerUnit->GetName(),playerVictim->GetName(),damage);
            }
            return damage;
    }

    uint32 HandlePeriodicDamageAurasTick(Unit *target, Unit *caster, int32 damage)
    {
        if (caster->GetMap()->IsDungeon() && target->GetMap()->IsDungeon())
            if (caster->GetTypeId() != TYPEID_PLAYER)
            {
                //if (GetValidDebugLevel() >= 3)
                    //sLog->outInfo("### AutoBalance_UnitScript - Unit_HandlePeriodicDamage Attacker=%s Victim=%s Start Damage=%u",caster->GetName(),target->GetName(),damage);

                if (!((caster->IsHunterPet() || caster->IsPet() || caster->IsSummon()) && caster->IsControlledByPlayer()))
                    damage = (float)damage * (float)CreatureInfo[caster->GetGUID()].DamageMultiplier;

                //if (GetValidDebugLevel() >= 3)
                    //sLog->outInfo("### AutoBalance_UnitScript - Unit_HandlePeriodicDamage Attacker=%s Victim=%s End Damage=%u",caster->GetName(),target->GetName(),damage);
            }
            return damage;
    }

    void CalculateSpellDamageTaken(SpellNonMeleeDamage *damageInfo, int32 damage, SpellInfo const *spellInfo, WeaponAttackType attackType, bool crit)
    {
        if ((damageInfo->attacker->GetMap()->IsDungeon() && damageInfo->target->GetMap()->IsDungeon()) || ( damageInfo->attacker->GetMap()->IsBattleground() && damageInfo->target->GetMap()->IsBattleground()))
        {
            if (damageInfo->attacker->GetTypeId() != TYPEID_PLAYER)
            {
                //if (GetValidDebugLevel() >= 3)
                    //sLog->outInfo("### AutoBalance_UnitScript - CalculateSpellDamageTaken Attacker=%s Victim=%s Start Damage=%u",damageInfo->attacker->GetName(),damageInfo->target->GetName(),damageInfo->damage);

                if ((damageInfo->attacker->IsHunterPet() || damageInfo->attacker->IsPet() || damageInfo->attacker->IsSummon()) && damageInfo->attacker->IsControlledByPlayer())
                    return;

                damageInfo->damage = (float)damageInfo->damage * (float)CreatureInfo[damageInfo->attacker->GetGUID()].DamageMultiplier;

                //if (GetValidDebugLevel() >= 3)
                    //sLog->outInfo("### AutoBalance_UnitScript - CalculateSpellDamageTaken Attacker=%s Victim=%s End Damage=%u",damageInfo->attacker->GetName(),damageInfo->target->GetName(),damageInfo->damage);
            }
        }
            return;
    }

    void CalculateMeleeDamage(Unit *playerVictim, uint32 damage, CalcDamageInfo *damageInfo, WeaponAttackType attackType)
    {
        // Make sure the Attacker and the Victim are in the same location, in addition that the attacker is not player.
        if (((damageInfo->Attacker->GetMap()->IsDungeon() && damageInfo->Target->GetMap()->IsDungeon()) || (damageInfo->Attacker->GetMap()->IsBattleground() && damageInfo->Target->GetMap()->IsBattleground())) && (damageInfo->Attacker->GetTypeId() != TYPEID_PLAYER))
            if (!((damageInfo->Attacker->IsHunterPet() || damageInfo->Attacker->IsPet() || damageInfo->Attacker->IsSummon()) && damageInfo->Attacker->IsControlledByPlayer())) // Make sure that the attacker Is not a Pet of some sort
            {
                //if (GetValidDebugLevel() >= 3)
                    //sLog->outInfo("### AutoBalance_UnitScript - CalculateMeleeDamage Attacker=%s Victim=%s Start Damage=%u",damageInfo->attacker->GetName(),damageInfo->target->GetName(),damageInfo->damage);

                damageInfo->Damages[0].Damage = (float)damageInfo->Damages[0].Damage * (float)CreatureInfo[damageInfo->Attacker->GetGUID()].DamageMultiplier;

                //if (GetValidDebugLevel() >= 3)
                    //sLog->outInfo("### AutoBalance_UnitScript - CalculateMeleeDamage Attacker=%s Victim=%s End Damage=%u",damageInfo->attacker->GetName(),damageInfo->target->GetName(),damageInfo->damage);
            }
            return;
    }

    uint32 Modifer_DealDamage(Unit* AttackerUnit,uint32 damage)
    {
        if ((AttackerUnit->IsHunterPet() || AttackerUnit->IsPet() || AttackerUnit->IsSummon()) && AttackerUnit->IsControlledByPlayer())
           return damage;

        float damageMultiplier = CreatureInfo[AttackerUnit->GetGUID()].DamageMultiplier;

        return damage * damageMultiplier;
    }
};


class AutoBalance_AllMapScript : public AllMapScript
{
    public:
    AutoBalance_AllMapScript() : AllMapScript("AutoBalance_AllMapScript") {}

        void OnPlayerEnterAll(Map* map, Player* player)
        {
            if (!enabled)
                return;

            AutoBalanceCreatureInfo *CreatureInfo = map->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");
            AutoBalanceMapInfo *MapInfo = map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            if (player->IsGameMaster())
                return;

            // CreatureInfo->playerCount++; (maybe we've to found a safe solution to avoid player recount each time)
            CreatureInfo->playerCount = map->GetPlayersCountExceptGMs();

            // always check level, even if not conf enabled
            // because we can enable at runtime and we need this information
            if (player)
            {
                if (player->getLevel() > MapInfo->mapLevel)
                    MapInfo->mapLevel = player->getLevel();
            }
            else
            {
                Map::PlayerList const &playerList = map->GetPlayers();

                if (!playerList.isEmpty())
                {
                    for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
                    {
                        if (Player* playerHandle = playerIteration->GetSource())
                        {
                            if (!playerHandle->IsGameMaster() && playerHandle->getLevel() > MapInfo->mapLevel)
                                MapInfo->mapLevel = playerHandle->getLevel();
                        }
                    }
                }
            }

            if (PlayerChangeNotify >= 1)
            {
                Map::PlayerList const &playerList = map->GetPlayers();
                if (!playerList.isEmpty())
                {
                    for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
                    {
                        if (Player* playerHandle = playerIteration->GetSource())
                        {
                            ChatHandler chatHandle = ChatHandler(playerHandle->GetSession());
                            chatHandle.PSendSysMessage("|cffFF0000 [AutoBalance]|r|cffFF8000 %s entered the Instance %s. Auto setting player count to %u |r",player->GetName().c_str(),map->GetMapName(),map->GetPlayersCountExceptGMs());
                        }
                    }
                }
            }
        }

        void OnPlayerLeaveAll(Map* map, Player* player)
        {
            if (!enabled)
                return;

            AutoBalanceCreatureInfo *CreatureInfo = map->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");
            AutoBalanceMapInfo *MapInfo = map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            if (player->IsGameMaster())
                return;

            // CreatureInfo->playerCount--; (maybe we've to found a safe solution to avoid player recount each time)
            CreatureInfo->playerCount = map->GetPlayersCountExceptGMs() - 1;

            // always check level, even if not conf enabled
            // because we can enable at runtime and we need this information
            if (player)
            {
                if (player->getLevel() > MapInfo->mapLevel)
                    MapInfo->mapLevel = player->getLevel();
            }
            else
            {
                Map::PlayerList const &playerList = map->GetPlayers();

                if (!playerList.isEmpty())
                {
                    for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
                    {
                        if (Player* playerHandle = playerIteration->GetSource())
                        {
                            if (!playerHandle->IsGameMaster() && playerHandle->getLevel() > MapInfo->mapLevel)
                                MapInfo->mapLevel = playerHandle->getLevel();
                        }
                    }
                }
            }

            if (PlayerChangeNotify >= 1)
            {
                Map::PlayerList const &playerList = map->GetPlayers();
                if (!playerList.isEmpty())
                {
                    for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
                    {
                        if (Player* playerHandle = playerIteration->GetSource())
                        {
                            ChatHandler chatHandle = ChatHandler(playerHandle->GetSession());
                            chatHandle.PSendSysMessage("|cffFF0000 [AutoBalance]|r|cffFF8000 %s left the Instance %s. Auto setting player count to %u (Player Difficulty Offset = %u) |r", player->GetName().c_str(), map->GetMapName(), CreatureInfo->playerCount, PlayerCountDifficultyOffset);
                        }
                    }
                }
            }
        }
};

class AutoBalance_AllCreatureScript : public AllCreatureScript
{
public:
    AutoBalance_AllCreatureScript() : AllCreatureScript("AutoBalance_AllCreatureScript") {}


    void Creature_SelectLevel(const CreatureTemplate* /*creatureTemplate*/, Creature* creature) override
    {
        if (!enabled)
            return;

        if (creature->GetMap()->IsDungeon())
        {
            ModifyCreatureAttributes(creature);
            CreatureInfo[creature->GetGUID()].playerCount = creature->GetMap()->GetPlayersCountExceptGMs();
        }
    }

    void OnAllCreatureUpdate(Creature* creature, uint32 /*diff*/) override
    {
        if (!enabled)
            return;

        if(!(CreatureInfo[creature->GetGUID()].playerCount == creature->GetMap()->GetPlayersCountExceptGMs()))
        {
            if (creature->GetMap()->IsDungeon() || creature->GetMap()->IsBattleground())
                ModifyCreatureAttributes(creature);
            CreatureInfo[creature->GetGUID()].playerCount = creature->GetMap()->GetPlayersCountExceptGMs();
        }
    }

    bool checkLevelOffset(uint8 mapLevel, uint8 targetLevel)
    {
        return mapLevel && ((targetLevel >= mapLevel && targetLevel <= (mapLevel + higherOffset) ) || (targetLevel <= mapLevel && targetLevel >= (mapLevel - lowerOffset)));
    }

    void ModifyCreatureAttributes(Creature* creature)
    {
        if(((creature->IsHunterPet() || creature->IsPet() || creature->IsSummon()) && creature->IsControlledByPlayer()) || sWorld->getIntConfig(_AutoInstance) < 1 || creature->GetMap()->GetPlayersCountExceptGMs() <= 0)
        {
            return;
        }

        CreatureTemplate const *creatureTemplate = creature->GetCreatureTemplate();
        CreatureBaseStats const* creatureStats = sObjectMgr->GetCreatureBaseStats(creature->getLevel(), creatureTemplate->unit_class);

        float damageMultiplier = 1.0f;
        float healthMultiplier = 1.0f;

        uint32 baseHealth = creatureStats->GenerateHealth(creatureTemplate);
        uint32 baseMana = creatureStats->GenerateMana(creatureTemplate);
        uint32 playerCount = creature->GetMap()->GetPlayersCountExceptGMs();
        uint32 maxNumberOfPlayers = ((InstanceMap*)sMapMgr->FindMap(creature->GetMapId(), creature->GetInstanceId()))->GetMaxPlayers();
        uint32 scaledHealth = 0;
        uint32 scaledMana = 0;

        //    SOLO  - By MobID
        if(GetForcedCreatureId(creatureTemplate->Entry) > 0)
        {
            maxNumberOfPlayers = GetForcedCreatureId(creatureTemplate->Entry); // Force maxNumberOfPlayers to be changed to match the Configuration entry.
        }

        // (tanh((X-2.2)/1.5) +1 )/2    // 5 Man formula X = Number of Players
        // (tanh((X-5)/2) +1 )/2        // 10 Man Formula X = Number of Players
        // (tanh((X-16.5)/6.5) +1 )/2   // 25 Man Formula X = Number of players
        //
        // Note: The 2.2, 5, and 16.5 are the number of players required to get 50% health.
        //       It's not required this be a whole number, you'd adjust this to raise or lower
        //       the hp modifier for per additional player in a non-whole group. These
        //       values will eventually be part of the configuration file once I finalize the mod.
        //
        //       The 1.5, 2, and 6.5 modify the rate of percentage increase between
        //       number of players. Generally the closer to the value of 1 you have this
        //       the less gradual the rate will be. For example in a 5 man it would take 3
        //       total players to face a mob at full health.
        //
        //       The +1 and /2 values raise the TanH function to a positive range and make
        //       sure the modifier never goes above the value or 1.0 or below 0.
        //
        //       Lastly this formula has one side effect on full groups Bosses and mobs will
        //       never have full health, this can be tested against by making sure the number
        //       of players match the maxNumberOfPlayers variable.

        switch (maxNumberOfPlayers)
        {
            case 40:
                healthMultiplier = (float)playerCount / (float)maxNumberOfPlayers; // 40 Man Instances oddly enough scale better with the old formula
                break;
            case 25:
                healthMultiplier = (tanh((playerCount - 16.5f) / 1.5f) + 1.0f) / 2.0f;
                break;
            case 10:
                healthMultiplier = (tanh((playerCount - 4.5f) / 1.5f) + 1.0f) / 2.0f;
                break;
            case 2:
                healthMultiplier = (float)playerCount / (float)maxNumberOfPlayers;                   // Two Man Creatures are too easy if handled by the 5 man formula, this would only
                break;                                                                         // apply in the situation where it's specified in the configuration file.
            default:
                healthMultiplier = (tanh((playerCount - 2.2f) / 1.5f) + 1.0f) / 2.0f;    // default to a 5 man group
        }

        //SOLO  - Map 0,1 and 530 ( World Mobs )
        // This may be where _AutoBalance_CheckINIMaps might have come into play. None the less this is
        // specific to World Bosses and elites in those Maps, this is going to use the entry XPlayer in place of playerCount.
        if((creature->GetMapId() == 0 || creature->GetMapId() == 1 || creature->GetMapId() == 530) && (creature->isElite() || creature->isWorldBoss()))
        {
            if(baseHealth > 800000)
            {
                healthMultiplier = (tanh((sWorld->getFloatConfig(_Config_xPlayer) - 5.0f) / 1.5f) + 1.0f) / 2.0f;
            }
            else
            {
                healthMultiplier = (tanh((sWorld->getFloatConfig(_Config_xPlayer) - 2.2f) / 1.5f) + 1.0f) / 2.0f; // Assuming a 5 man configuration, as World Bosses have been relatively retired since BC so unless the boss has some substantial baseHealth
            }

        }

        // Ensure that the healthMultiplier is not lower than the configuration specified value. -- This may be Deprecated later.
        if(healthMultiplier <= sWorld->getFloatConfig(_Min_HP_Mod) )
        {
            healthMultiplier = sWorld->getFloatConfig(_Min_HP_Mod);
        }

        //Getting the list of Classes in this group - this will be used later on to determine what additional scaling will be required based on the ratio of tank/dps/healer
        //GetPlayerClassList(creature, playerClassList); // Update playerClassList with the list of all the participating Classes

        scaledHealth = uint32((baseHealth * healthMultiplier) + 1.0f);
        // Now adjusting Mana, Mana is something that can be scaled linearly
        if (maxNumberOfPlayers==0)
        {
            scaledMana = uint32((baseMana * healthMultiplier) + 1.0f);
            // Now Adjusting Damage, this too is linear for now .... this will have to change I suspect.
            damageMultiplier = healthMultiplier;
        }
        else
        {
            scaledMana = ((baseMana/maxNumberOfPlayers) * playerCount);
            // Now Adjusting Damage, this too is linear for now .... this will have to change I suspect.
            damageMultiplier = (float)playerCount / (float)maxNumberOfPlayers;
        }

        // Can not be less then Min_D_Mod
        if(damageMultiplier <= sWorld->getFloatConfig(_Min_D_Mod))
        {
            damageMultiplier = sWorld->getFloatConfig(_Min_D_Mod);
        }

        creature->SetCreateHealth(scaledHealth);
        creature->SetMaxHealth(scaledHealth);
        creature->ResetPlayerDamageReq();
        creature->SetCreateMana(scaledMana);
        creature->SetMaxPower(POWER_MANA, scaledMana);
        creature->SetPower(POWER_MANA, scaledMana);
        creature->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, (float)scaledHealth);
        creature->SetStatFlatModifier(UNIT_MOD_MANA, BASE_VALUE, (float)scaledMana);
        CreatureInfo[creature->GetGUID()].DamageMultiplier = damageMultiplier;
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
                { "setoffset",       SEC_GAMEMASTER, true, &HandleABSetOffsetCommand,        "Sets the global Player Difficulty Offset for instances. Example: (You + offset(1) = 2 player difficulty)." },
                { "getoffset",       SEC_GAMEMASTER, true, &HandleABGetOffsetCommand,        "Shows current global player offset value"                                                                  },
                { "checkmap",        SEC_GAMEMASTER, true, &HandleABCheckMapCommand,         "Run a check for current map/instance, it can help in case you're testing autobalance with GM."             },
                { "mapstat",         SEC_GAMEMASTER, true, &HandleABMapStatsCommand,         "Shows current autobalance information for this map-"                                                       },
                { "creaturestat",    SEC_GAMEMASTER, true, &HandleABCreatureStatsCommand,    "Shows current autobalance information for selected creature."                                              },
            };

            static std::vector<ChatCommand> commandTable =
            {
                { "autobalance",     SEC_GAMEMASTER, false, NULL, "", ABCommandTable },
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

            AutoBalanceCreatureInfo *CreatureInfo = pl->GetMap()->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");
            AutoBalanceMapInfo *MapInfo = creature->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
            CreatureInfo->playerCount = pl->GetMap()->GetPlayersCountExceptGMs();
            Map::PlayerList const &playerList = pl->GetMap()->GetPlayers();
            uint8 level = 0;

            if (!playerList.isEmpty())
            {
                for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
                {
                    if (Player* playerHandle = playerIteration->GetSource())
                    {
                        if (playerHandle->getLevel() > level)
                            MapInfo->mapLevel = level = playerHandle->getLevel();
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

            AutoBalanceCreatureInfo *CreatureInfo = pl->GetMap()->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");
            AutoBalanceMapInfo *MapInfo = creature->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            handler->PSendSysMessage("Players on map: %u", CreatureInfo->playerCount);
            handler->PSendSysMessage("Max level of players in this map: %u", MapInfo->mapLevel);

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

            AutoBalanceCreatureInfo *CreatureInfo = target->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

            handler->PSendSysMessage("Instance player Count: %u", CreatureInfo->playerCount);
            handler->PSendSysMessage("Selected level: %u", CreatureInfo->mapLevel);
            handler->PSendSysMessage("Damage multiplier: %.6f", CreatureInfo->DamageMultiplier);
            handler->PSendSysMessage("Health multiplier: %.6f", CreatureInfo->HealthMultiplier);
            handler->PSendSysMessage("Mana multiplier: %.6f", CreatureInfo->ManaMultiplier);
            handler->PSendSysMessage("Armor multiplier: %.6f", CreatureInfo->ArmorMultiplier);

            return true;

        }
};

class AutoBalance_GlobalScript : public GlobalScript
{
    public:
        AutoBalance_GlobalScript() : GlobalScript("AutoBalance_GlobalScript") {}

        void OnAfterUpdateEncounterState(Map* map, EncounterCreditType type,  uint32 /*creditEntry*/, Unit* source, Difficulty /*difficulty_fixed*/, DungeonEncounterList const* /*encounters*/, uint32 /*dungeonCompleted*/, bool updated) override {
        //if (!dungeonCompleted)
        //    return;

        if (!rewardEnabled || !updated)
            return;

        if (map->GetPlayersCountExceptGMs() < MinPlayerReward)
            return;

        AutoBalanceCreatureInfo *mapInfo = map->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

        uint8 areaMinLvl, areaMaxLvl;
        getAreaLevel(map, source->GetAreaId(), areaMinLvl, areaMaxLvl);

        // skip if it's not a pre-wotlk dungeon/raid and if it's not scaled
        if (!LevelScaling || lowerOffset >= 10 || MapInfo->mapLevel <= 70 || areaMinLvl > 70
            // skip when not in dungeon or not kill credit
            || type != ENCOUNTER_CREDIT_KILL_CREATURE || !map->IsDungeon())
            return;

        Map::PlayerList const &playerList = map->GetPlayers();

        if (playerList.isEmpty())
            return;

        uint32 reward = map->IsRaid() ? rewardRaid : rewardDungeon;
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
    new AutoBalance_WorldScript;
    new AutoBalance_PlayerScript;
    new AutoBalance_UnitScript;
    new AutoBalance_AllCreatureScript;
    new AutoBalance_AllMapScript;
    new AutoBalance_CommandScript;
    new AutoBalance_GlobalScript;
}
