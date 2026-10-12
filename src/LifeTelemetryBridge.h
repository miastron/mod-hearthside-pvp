/*
 * Playerbots Life Dashboard server telemetry bridge
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "LifeBotReservation.h"
#include "Chat.h"
#include "GameTime.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldScript.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace PlayerbotsLife
{
    inline std::uint32_t CalculateCRC32(std::string_view data)
    {
        std::uint32_t crc = 0xFFFFFFFF;
        for (unsigned char c : data)
        {
            std::uint32_t entry = (crc ^ c) & 0xFF;
            for (int j = 0; j < 8; ++j)
            {
                if (entry & 1)
                    entry = (entry >> 1) ^ 0xEDB88320;
                else
                    entry >>= 1;
            }
            crc = (crc >> 8) ^ entry;
        }
        return crc ^ 0xFFFFFFFF;
    }

    inline std::string CalculateCRC32Hex(std::string_view data)
    {
        std::uint32_t crc = CalculateCRC32(data);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08X", crc);
        return std::string(buf);
    }

    inline std::string EscapeTelemetry(std::string_view data)
    {
        std::string out;
        out.reserve(data.size());
        char hex[4];
        for (unsigned char c : data)
        {
            if (c == '|' || c == '\t' || c == '\n' || c == '\r' || c == '%' || c < 32)
            {
                std::snprintf(hex, sizeof(hex), "%%%02X", c);
                out.append(hex);
            }
            else
            {
                out.push_back(static_cast<char>(c));
            }
        }
        return out;
    }

    inline std::string UnescapeTelemetry(std::string_view data)
    {
        std::string out;
        out.reserve(data.size());
        for (std::size_t i = 0; i < data.size(); ++i)
        {
            if (data[i] == '%' && i + 2 < data.size())
            {
                char hex[3] = { data[i + 1], data[i + 2], '\0' };
                char* end = nullptr;
                long val = std::strtol(hex, &end, 16);
                if (end == hex + 2)
                {
                    out.push_back(static_cast<char>(val));
                    i += 2;
                    continue;
                }
            }
            out.push_back(data[i]);
        }
        return out;
    }

    class LifeTelemetryBridge
    {
    public:
        static LifeTelemetryBridge& Instance()
        {
            static LifeTelemetryBridge instance;
            return instance;
        }

        void OnPlayerLogout(uint32 guidLow)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _subscribers.erase(guidLow);
        }

        static void SendTelemetryFrame(Player* player, std::string const& wireFrame)
        {
            if (!player || !player->GetSession())
                return;

            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, "LIFE335\t" + wireFrame);
            player->GetSession()->SendPacket(&data);
        }

        static void SendTelemetryMessage(Player* player, std::string const& kind, std::string const& stream,
                                         uint32 reqId, std::string const& session, std::string const& rawPayload)
        {
            if (!player || !player->GetSession())
                return;

            std::string crc = CalculateCRC32Hex(rawPayload);
            uint32 totalBytes = static_cast<uint32>(rawPayload.size());
            std::string escaped = EscapeTelemetry(rawPayload);

            std::string singleHeader = "1|" + kind + "|" + session + "|" + stream + "|1|" +
                                       std::to_string(reqId) + "|1|1|" +
                                       std::to_string(totalBytes) + "|" + crc + "|";
            if (singleHeader.size() + escaped.size() <= 240)
            {
                SendTelemetryFrame(player, singleHeader + escaped);
                return;
            }

            constexpr std::size_t MAX_WIRE_BYTES = 240;
            std::size_t maxBodyPerChunk = 150;
            std::vector<std::string> chunks;
            for (std::size_t offset = 0; offset < escaped.size(); offset += maxBodyPerChunk)
                chunks.push_back(escaped.substr(offset, maxBodyPerChunk));

            uint32 totalParts = static_cast<uint32>(chunks.size());
            for (uint32 part = 1; part <= totalParts; ++part)
            {
                std::string partHeader = "1|" + kind + "|" + session + "|" + stream + "|1|" +
                                         std::to_string(reqId) + "|" +
                                         std::to_string(part) + "|" +
                                         std::to_string(totalParts) + "|" +
                                         std::to_string(totalBytes) + "|" + crc + "|";
                std::string wireFrame = partHeader + chunks[part - 1];
                if (wireFrame.size() <= MAX_WIRE_BYTES)
                    SendTelemetryFrame(player, wireFrame);
            }
        }

        bool HandleClientMessage(Player* player, std::string const& rawFrame)
        {
            if (!player || !player->GetSession())
                return false;

            // Header grammar: V|kind|session|stream|seq|request|part|parts|bytes|crc|body
            std::vector<std::string> tokens;
            tokens.reserve(11);
            std::size_t start = 0;
            for (int i = 0; i < 10; ++i)
            {
                std::size_t pipe = rawFrame.find('|', start);
                if (pipe == std::string::npos)
                    return false;
                tokens.push_back(rawFrame.substr(start, pipe - start));
                start = pipe + 1;
            }
            tokens.push_back(rawFrame.substr(start));

            if (tokens.size() < 11)
                return false;

            std::string const& versionStr = tokens[0];
            std::string const& kind       = tokens[1];
            std::string const& session    = tokens[2];
            std::string const& stream     = tokens[3];
            std::string const& reqStr     = tokens[5];
            std::string const& body       = tokens[10];

            if (versionStr != "1")
                return false;

            uint32 reqId = static_cast<uint32>(std::strtoul(reqStr.c_str(), nullptr, 10));
            std::string unescapedBody = UnescapeTelemetry(body);

            if (kind == "HELLO")
            {
                std::string tier = (player->GetSession()->GetSecurity() > SEC_PLAYER) ? "admin" : "player";
                std::string welcomePayload = "all|" + tier + "|" +
                    std::to_string(GameTime::GetGameTimeMS()) + "|" +
                    std::to_string(GameTime::GetGameTime());
                SendTelemetryMessage(player, "WELCOME", "main", reqId, "1", welcomePayload);

                CooperationSnapshot snap = GetCooperationSnapshot(GameTimeSeconds());
                std::string capPayload = BuildCapacityPayload(snap);
                SendTelemetryMessage(player, "CAPACITY", "main", 0, "1", capPayload);
                return true;
            }
            else if (kind == "PING")
            {
                SendTelemetryMessage(player, "PONG", stream, reqId, session, unescapedBody);
                return true;
            }
            else if (kind == "SUBSCRIBE")
            {
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    _subscribers.insert(player->GetGUID().GetCounter());
                }
                CooperationSnapshot snap = GetCooperationSnapshot(GameTimeSeconds());
                std::string capPayload = BuildCapacityPayload(snap);
                SendTelemetryMessage(player, "CAPACITY", "main", reqId, "1", capPayload);
                return true;
            }
            else if (kind == "UNSUBSCRIBE")
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _subscribers.erase(player->GetGUID().GetCounter());
                return true;
            }

            return true;
        }

        void Update(uint32 diff)
        {
            _broadcastTimerMs += diff;
            if (_broadcastTimerMs < 5000)
                return;
            _broadcastTimerMs = 0;

            std::vector<uint32> activeSubs;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (_subscribers.empty())
                    return;
                activeSubs.assign(_subscribers.begin(), _subscribers.end());
            }

            CooperationSnapshot snap = GetCooperationSnapshot(GameTimeSeconds());
            std::string capPayload = BuildCapacityPayload(snap);

            for (uint32 guidLow : activeSubs)
            {
                Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guidLow));
                if (!player || !player->IsInWorld())
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    _subscribers.erase(guidLow);
                    continue;
                }
                SendTelemetryMessage(player, "CAPACITY", "main", 0, "1", capPayload);
            }
        }

    private:
        LifeTelemetryBridge() = default;

        static std::string BuildCapacityPayload(CooperationSnapshot const& snap)
        {
            uint32 freeCapacity = 0;
            return std::to_string(snap.TotalReserved) + "|" +
                   std::to_string(snap.CityLifeCount) + "|" +
                   std::to_string(snap.ActiveCount) + "|" +
                   std::to_string(snap.ProvisionalCount) + "|" +
                   std::to_string(snap.ReleasingCount) + "|" +
                   std::to_string(snap.PvPLifeCount) + "|" +
                   std::to_string(snap.ActiveCount) + "|" +
                   std::to_string(snap.ProvisionalCount) + "|" +
                   std::to_string(snap.ReleasingCount) + "|" +
                   std::to_string(freeCapacity);
        }

        std::mutex _mutex;
        std::unordered_set<uint32> _subscribers;
        uint32 _broadcastTimerMs{0};
    };

    class LifeTelemetryPlayerScript : public PlayerScript
    {
    public:
        LifeTelemetryPlayerScript()
            : PlayerScript("LifeTelemetryPlayerScript", { PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT })
        {
        }

        void OnPlayerLogout(Player* player) override
        {
            if (player)
                LifeTelemetryBridge::Instance().OnPlayerLogout(player->GetGUID().GetCounter());
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
        {
            if (type == CHAT_MSG_WHISPER && language == LANG_ADDON && receiver == player)
            {
                if (msg.rfind("LIFE335\t", 0) == 0)
                {
                    std::string frame = msg.substr(8);
                    LifeTelemetryBridge::Instance().HandleClientMessage(player, frame);
                    return false;
                }
            }
            return true;
        }
    };

    class LifeTelemetryWorldScript : public WorldScript
    {
    public:
        LifeTelemetryWorldScript()
            : WorldScript("LifeTelemetryWorldScript", { WORLDHOOK_ON_UPDATE })
        {
        }

        void OnUpdate(uint32 diff) override
        {
            LifeTelemetryBridge::Instance().Update(diff);
        }
    };

    inline void RegisterTelemetryBridge()
    {
        ReservationRegistry& registry = GetReservationRegistry();
        if (!registry.TelemetryBridgeRegistered.exchange(true))
        {
            new LifeTelemetryPlayerScript();
            new LifeTelemetryWorldScript();
        }
    }
}
