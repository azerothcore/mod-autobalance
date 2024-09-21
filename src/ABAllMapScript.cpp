#include "Chat.h"

#include "ABAllMapScript.h"
#include "ABConfig.h"
#include "ABMapInfo.h"
#include "ABUtils.h"
#include "Message.h"

void AutoBalance_AllMapScript::OnCreateMap(Map* map)
{
    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnCreateMap(): Map {} ({}{})",
        map->GetMapName(),
        map->GetId(),
        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
    );

    // clear out any previously-recorded data
    map->CustomData.Erase("AutoBalanceMapInfo");

    AutoBalanceMapInfo* mapABInfo = map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

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

void AutoBalance_AllMapScript::OnPlayerEnterAll(Map* map, Player* player)
{
    if (!EnableGlobal)
        return;

    if (!map->IsDungeon())
        return;

    LOG_DEBUG("module.AutoBalance", "AutoBalance:: ------------------------------------------------");

    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnPlayerEnterAll: Player {}{} | enters {} ({}{})",
        player->GetName(),
        player->IsGameMaster() ? " (GM)" : "",
        map->GetMapName(),
        map->GetId(),
        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
    );

    // get the map's info
    AutoBalanceMapInfo* mapABInfo = map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

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
    if ((prevAdjustedPlayerCount == mapABInfo->adjustedPlayerCount) && (mapABInfo->adjustedPlayerCount != 1))
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

void AutoBalance_AllMapScript::OnPlayerLeaveAll(Map* map, Player* player)
{
    if (!EnableGlobal)
        return;

    if (!map->IsDungeon())
        return;

    LOG_DEBUG("module.AutoBalance", "AutoBalance:: ------------------------------------------------");

    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnPlayerLeaveAll: Player {}{} | exits {} ({}{})",
        player->GetName(),
        player->IsGameMaster() ? " (GM)" : "",
        map->GetMapName(),
        map->GetId(),
        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
    );

    // get the map's info
    AutoBalanceMapInfo* mapABInfo = map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

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
