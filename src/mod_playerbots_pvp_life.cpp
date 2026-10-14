/*
 * PvP Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PvPLifeMgr.h"
#include "../../mod-hearthside-life/src/LifeBotReservation.h"

#include "Chat.h"
#include "CommandScript.h"
#include "Group.h"
#include "GroupScript.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldScript.h"

using namespace Acore::ChatCommands;

class PvPLifeWorldScript : public WorldScript
{
public:
    PvPLifeWorldScript()
        : WorldScript("PvPLifeWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN })
    {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sPvPLifeMgr.LoadConfig();
    }

    void OnUpdate(uint32 diff) override
    {
        sPvPLifeMgr.Update(diff);
    }

    void OnShutdown() override
    {
        sPvPLifeMgr.Shutdown();
        PlayerbotsLife::UnregisterActiveModule(PlayerbotsLife::MODULE_PVP_LIFE);
    }
};

class PvPLifePlayerScript : public PlayerScript
{
public:
    PvPLifePlayerScript()
        : PlayerScript("PvPLifePlayerScript", { PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_DUEL_END })
    {
    }

    void OnPlayerLogout(Player* player) override
    {
        sPvPLifeMgr.OnPlayerLogout(player);
    }

    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
    {
        if (type == DUEL_WON)
            sPvPLifeMgr.QueueDuelEnd(winner, loser);
    }
};

class PvPLifeGroupScript : public GroupScript
{
public:
    PvPLifeGroupScript()
        : GroupScript("PvPLifeGroupScript", { GROUPHOOK_ON_ADD_MEMBER })
    {
    }

    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        if (!group)
            return;

        bool hasRealPlayer = false;
        for (GroupReference const* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
        {
            // A SelfBot is a person who switched on bot control: still a human taking the bot over.
            if (PvPLife::IsHumanPlayer(ref->GetSource()) || ::IsSelfBot(ref->GetSource()))
            {
                hasRealPlayer = true;
                break;
            }
        }

        if (hasRealPlayer)
        {
            sPvPLifeMgr.OnAddGroupMember(guid.GetCounter());
            for (GroupReference const* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
            {
                Player const* member = ref->GetSource();
                if (PvPLife::IsBotSession(member))
                    sPvPLifeMgr.OnAddGroupMember(member->GetGUID().GetCounter());
            }
        }
    }
};

class PvPLifeCommandScript : public CommandScript
{
public:
    PvPLifeCommandScript() : CommandScript("PvPLifeCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "pvplife", HandlePvPLifeCommand, SEC_GAMEMASTER, Console::Yes },
            { "wpvp",    HandleWpvpCommand,    SEC_GAMEMASTER, Console::Yes }
        };
        return commandTable;
    }

    static bool HandlePvPLifeCommand(ChatHandler* handler, char const* args)
    {
        return sPvPLifeMgr.HandleCommand(handler, args ? args : "");
    }

    static bool HandleWpvpCommand(ChatHandler* handler, char const* args)
    {
        return sPvPLifeMgr.HandleWpvpCommand(handler, args ? args : "");
    }
};

void AddPvPLifeScripts()
{
    // A sanity check: every Life module compiles mod-hearthside-life's one LifeBotReservation.h, so
    // this only fails if a module was built against an older copy.
    if (!PlayerbotsLife::CheckProtocolVersion())
        LOG_ERROR("server.loading", ">> PvP Life: LifeBotReservation.h protocol {} does not match {} from another Life "
            "module. Rebuild every Life module against mod-hearthside-life.",
            PlayerbotsLife::LIFE_BOT_RESERVATION_PROTOCOL_VERSION, PlayerbotsLife::GetRegisteredProtocolVersion());

    // The active-module flag is set by Manager::LoadConfig, only while the module is enabled and
    // its tables check out.
    new PvPLifeWorldScript();
    new PvPLifePlayerScript();
    new PvPLifeGroupScript();
    new PvPLifeCommandScript();
    LOG_INFO("server.loading", ">> Loaded mod-playerbots-pvp-life (protocol={}, registry={:p})",
        PlayerbotsLife::GetReservationProtocolVersion(), PlayerbotsLife::GetReservationRegistryAddress());
}
