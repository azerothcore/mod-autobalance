/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE
 */

#ifndef __AB_COMMAND_SCRIPT_H
#define __AB_COMMAND_SCRIPT_H

#include "Chat.h"
#include "Config.h"
#include "ScriptMgr.h"

class AutoBalance_CommandScript : public CommandScript
{
public:
    AutoBalance_CommandScript() : CommandScript("AutoBalance_CommandScript") { }

    std::vector<Acore::ChatCommands::ChatCommandBuilder> GetCommands() const
    {
        static std::vector<Acore::ChatCommands::ChatCommandBuilder> ABCommandTable =
        {
            { "setoffset",     HandleABSetOffsetCommand,      SEC_GAMEMASTER,  Acore::ChatCommands::Console::Yes },
            { "getoffset",     HandleABGetOffsetCommand,      SEC_PLAYER,      Acore::ChatCommands::Console::Yes },
            { "mapstat",       HandleABMapStatsCommand,       SEC_PLAYER,      Acore::ChatCommands::Console::Yes },
            { "creaturestat",  HandleABCreatureStatsCommand,  SEC_PLAYER,      Acore::ChatCommands::Console::Yes }
        };

        static std::vector<Acore::ChatCommands::ChatCommandBuilder> commandTable =
        {
            { "autobalance",  ABCommandTable },
            { "ab",           ABCommandTable },
        };

        return commandTable;
    };

    static bool HandleABSetOffsetCommand(ChatHandler* handler, const char* args);
    static bool HandleABGetOffsetCommand(ChatHandler* handler, const char* args);
    static bool HandleABMapStatsCommand(ChatHandler* handler, const char* args);
    static bool HandleABCreatureStatsCommand(ChatHandler* handler, const char* args);
};

#endif /* __AB_COMMAND_SCRIPT_H */
