#include "ScriptMgr.h"
#include "Configuration/Config.h"
#include "ObjectMgr.h"
#include "Chat.h"
#include "Player.h"
#include "Object.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "WorldSession.h"
#include "StringConvert.h"
#include "Log.h"
#include "Timer.h"
#include "WorldPacket.h"
#if __has_include("ChallengesApi.h")
#include "ChallengesApi.h"
#endif
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

using namespace Acore::ChatCommands;

/*
Coded by Talamortis - For Azerothcore
Thanks to Rochet for the assistance
*/

struct IndividualXpModule
{
    bool Enabled, AnnounceModule, AnnounceRatesOnLogin;
    float MaxRate, DefaultRate;
    // A chosen rate above RampStartRate grows with the character's level: RampStartRate at level 1,
    // the full rate from RampFullLevel on. RampFullLevel 0 or 1 turns the ramp off.
    float RampStartRate;
    uint32 RampFullLevel;
};

IndividualXpModule individualXp;

enum IndividualXPAcoreString
{
    ACORE_STRING_CREDIT = 100000,
    ACORE_STRING_MODULE_DISABLED,
    ACORE_STRING_RATES_DISABLED,
    ACORE_STRING_COMMAND_VIEW,
    ACORE_STRING_MAX_RATE,
    ACORE_STRING_MIN_RATE,
    ACORE_STRING_COMMAND_SET,
    ACORE_STRING_COMMAND_DISABLED,
    ACORE_STRING_COMMAND_ENABLED,
    ACORE_STRING_COMMAND_DEFAULT,
    ACORE_STRING_RAMP
};

// The minimum rate .xp set accepts.
constexpr float MIN_RATE = 0.1f;

class IndividualXPConf : public WorldScript
{
public:
    IndividualXPConf() : WorldScript("IndividualXPConf") { }

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        individualXp.Enabled = sConfigMgr->GetOption<bool>("IndividualXp.Enabled", true);
        individualXp.AnnounceModule = sConfigMgr->GetOption<bool>("IndividualXp.Announce", true);
        individualXp.AnnounceRatesOnLogin = sConfigMgr->GetOption<bool>("IndividualXp.AnnounceRatesOnLogin", true);
        individualXp.MaxRate = sConfigMgr->GetOption<float>("IndividualXp.MaxXPRate", 10.0f);
        individualXp.DefaultRate = sConfigMgr->GetOption<float>("IndividualXp.DefaultXPRate", 1.0f);

        // A start rate near 0 would round level 1's XP down to nothing and leave the character stuck there.
        individualXp.RampStartRate = sConfigMgr->GetOption<float>("IndividualXp.RampStartRate", 1.0f);
        if (!(individualXp.RampStartRate >= MIN_RATE))
        {
            LOG_WARN("module", "IndividualXp.RampStartRate {} is below {}, using {}.", individualXp.RampStartRate, MIN_RATE, MIN_RATE);
            individualXp.RampStartRate = MIN_RATE;
        }

        int32 const rampFullLevel = sConfigMgr->GetOption<int32>("IndividualXp.RampFullLevel", 30);
        individualXp.RampFullLevel = uint32(std::clamp<int32>(rampFullLevel, 0, STRONG_MAX_LEVEL));
        if (int32(individualXp.RampFullLevel) != rampFullLevel)
            LOG_WARN("module", "IndividualXp.RampFullLevel {} is out of range, using {}.", rampFullLevel, individualXp.RampFullLevel);
    }
};

class PlayerXpRate : public DataMap::Base
{
public:
    PlayerXpRate() {}
    PlayerXpRate(float XPRate) : XPRate(XPRate) {}
    float XPRate = 1.0f;
    // Set once the player changes the rate, so a late login load does not overwrite it.
    bool Changed = false;
    // True when the character follows DefaultXPRate (no stored row). Unknown (false) until the
    // login load or a change, so an early DEFAULT from the addon still deletes the row.
    bool FollowsDefault = false;
    // getMSTime() of the last addon request, to throttle them (see OnPlayerCanUseChat).
    uint32 LastAddonRequest = 0;
};

// Rates changed since startup, by character. Async writes on several database workers
// are not ordered against a later login's read, so the login load checks this first.
// A rate <= 0 means "use DefaultXPRate" (the row was deleted).
std::unordered_map<uint32, float> ChangedRates;
std::mutex ChangedRatesLock;

// Stores a rate the player chose. The write is async and only happens when
// the rate actually changes, never on login or logout. useDefault deletes the
// row, so the character follows DefaultXPRate from then on.
void SetPlayerXpRate(Player* player, float rate, bool useDefault = false)
{
    PlayerXpRate* data = player->CustomData.GetDefault<PlayerXpRate>("IndividualXP");
    data->XPRate = rate;
    data->Changed = true;
    data->FollowsDefault = useDefault;

    uint32 const guid = player->GetGUID().GetCounter();
    {
        std::lock_guard<std::mutex> lock(ChangedRatesLock);
        ChangedRates[guid] = useDefault ? 0.0f : rate;
    }

    if (useDefault)
        CharacterDatabase.Execute("DELETE FROM `individualxp` WHERE `CharacterGUID`='{}'", guid);
    else
        CharacterDatabase.Execute("REPLACE INTO `individualxp` (`CharacterGUID`, `XPRate`) VALUES ('{}', '{}')", guid, rate);
}

float GetPlayerXpRate(Player* player)
{
    return player->CustomData.GetDefault<PlayerXpRate>("IndividualXP")->XPRate;
}

// The highest rate the player may have: MaxXPRate, or less while a challenge caps experience
// (mod-challenges' Hardcore: 1x).
float GetMaxXpRate(Player* player)
{
    float max = individualXp.MaxRate;
#if __has_include("ChallengesApi.h")
    if (float const cap = Challenges::XpRateCap(player->GetGUID().GetCounter()))
        max = std::min(max, std::max(MIN_RATE, cap));
#endif
    return max;
}

// The rate that holds: the chosen one, held to today's maximum. The stored choice stays as it is,
// so it comes back once the cap lifts.
float GetHeldXpRate(Player* player)
{
    return std::min(GetPlayerXpRate(player), GetMaxXpRate(player));
}

bool IsRamping(Player* player, float rate)
{
    return individualXp.RampFullLevel > 1 && rate > individualXp.RampStartRate
        && player->GetLevel() < individualXp.RampFullLevel;
}

// The rate a kill or quest is paid at right now. While ramping it climbs in a straight line with
// the character's level, counting the part of the current level already done, so it never jumps:
// a chosen 5x is 1x at level 1, 3x halfway to RampFullLevel and 5x from RampFullLevel on.
float GetEffectiveXpRate(Player* player, float rate)
{
    if (!IsRamping(player, rate))
        return rate;

    float progress = float(player->GetLevel() - 1);
    if (uint32 const next = player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP))
        progress += std::min(1.0f, float(player->GetUInt32Value(PLAYER_XP)) / float(next));

    float const start = individualXp.RampStartRate;
    return start + (rate - start) * std::min(1.0f, progress / float(individualXp.RampFullLevel - 1));
}

// Rounded to one decimal, for chat.
float ForChat(float rate)
{
    return std::round(rate * 10.0f) / 10.0f;
}

void SendRampLine(ChatHandler* handler, Player* player, float rate)
{
    if (IsRamping(player, rate))
        handler->PSendSysMessage(ACORE_STRING_RAMP, individualXp.RampFullLevel, ForChat(GetEffectiveXpRate(player, rate)));
}

// DragonUI's XP rate setting (its modules/xprate.lua) talks to the module with addon whispers to
// the player under this prefix:
//   server -> client   R:<rate>:<max>:<min>:<rampStart>:<rampFullLevel>:<off>
//                      the chosen rate and what the setting may offer; off is 1 while XP gain is
//                      turned off. The client works the ramp out itself from the level and XP.
//                      Sent after the stored rate loads at login, on HELLO and after every change.
//   client -> server   HELLO          send R again (the UI was reloaded)
//                      SET:<rate>     choose a rate, like .xp set
//                      DEFAULT        follow DefaultXPRate again, like .xp default
//                      ON / OFF       like .xp enable / .xp disable
constexpr char const* ADDON_PREFIX = "DUI_XPRATE";
constexpr uint32 ADDON_REQUEST_COOLDOWN = 100; // ms

void SendStateToAddon(Player* player)
{
    if (!individualXp.Enabled || player->GetSession()->IsHeadless())
        return;

    std::string const message = Acore::StringFormat("{}\tR:{}:{}:{}:{}:{}:{}", ADDON_PREFIX, GetHeldXpRate(player),
        GetMaxXpRate(player), MIN_RATE, individualXp.RampStartRate, individualXp.RampFullLevel,
        player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN) ? 1 : 0);
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
    player->GetSession()->SendPacket(&data);
}

void AnnounceRates(Player* player)
{
    if (!individualXp.Enabled || !individualXp.AnnounceRatesOnLogin)
        return;

    if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
    {
        ChatHandler(player->GetSession()).PSendSysMessage(ACORE_STRING_RATES_DISABLED);
    }
    else
    {
        ChatHandler handler(player->GetSession());
        handler.PSendSysMessage(ACORE_STRING_COMMAND_VIEW, GetHeldXpRate(player));
        SendRampLine(&handler, player, GetHeldXpRate(player));
        handler.PSendSysMessage(ACORE_STRING_MAX_RATE, GetMaxXpRate(player));
    }
}

class IndividualXP : public PlayerScript
{
public:
    IndividualXP() : PlayerScript("IndividualXP") {}

    void OnPlayerLogin(Player* player) override
    {
        // Use the default rate until the stored one has loaded.
        player->CustomData.GetDefault<PlayerXpRate>("IndividualXP")->XPRate = individualXp.DefaultRate;

        WorldSession* session = player->GetSession();
        bool const announce = !session->IsHeadless(); // bots get no chat lines

        if (announce && individualXp.Enabled && individualXp.AnnounceModule)
        {
            ChatHandler(session).SendSysMessage(ACORE_STRING_CREDIT);
        }

        ObjectGuid const guid = player->GetGUID();
        session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(Acore::StringFormat(
            "SELECT `XPRate` FROM `individualxp` WHERE `CharacterGUID`='{}'", guid.GetCounter()))
            .WithCallback([session, guid, announce](QueryResult result)
            {
                Player* player = session->GetPlayer();
                if (!player || player->GetGUID() != guid)
                    return;

                PlayerXpRate* data = player->CustomData.GetDefault<PlayerXpRate>("IndividualXP");
                if (!data->Changed)
                {
                    std::lock_guard<std::mutex> lock(ChangedRatesLock);
                    if (auto itr = ChangedRates.find(guid.GetCounter()); itr != ChangedRates.end())
                    {
                        data->XPRate = itr->second > 0.0f ? itr->second : individualXp.DefaultRate;
                        data->FollowsDefault = itr->second <= 0.0f;
                    }
                    else if (result)
                    {
                        // A row from before MaxXPRate was lowered still keeps to today's limits.
                        data->XPRate = std::clamp(result->Fetch()[0].Get<float>(), MIN_RATE, std::max(MIN_RATE, individualXp.MaxRate));
                        data->FollowsDefault = false;
                    }
                    else
                        data->FollowsDefault = true;
                }

                if (announce)
                {
                    AnnounceRates(player);
                    SendStateToAddon(player);
                }
            }));
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (individualXp.Enabled)
        {
            if (PlayerXpRate* data = player->CustomData.Get<PlayerXpRate>("IndividualXP"))
            {
                amount = static_cast<uint32>(std::round(static_cast<float>(amount) * GetEffectiveXpRate(player,
                    std::min(data->XPRate, GetMaxXpRate(player)))));
            }
        }
    }

    // The addon whispers itself. Every whisper with our prefix is answered here and swallowed.
    // The core lets through 100 addon messages a second, too many for database writes, so a
    // change that comes within ADDON_REQUEST_COOLDOWN of the last one is not made. It is still
    // answered, so the client shows the rate that holds. DragonUI spaces its own messages
    // further apart than that, but server lag can bunch them up.
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (lang != LANG_ADDON || receiver != player)
            return true;

        std::string const prefixTab = std::string(ADDON_PREFIX) + "\t";
        if (msg.compare(0, prefixTab.size(), prefixTab) != 0)
            return true;

        if (!individualXp.Enabled)
            return false;

        std::string_view const request = std::string_view(msg).substr(prefixTab.size());
        bool const xpOff = player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);

        PlayerXpRate* data = player->CustomData.GetDefault<PlayerXpRate>("IndividualXP");
        uint32 const now = getMSTime();
        if (request != "HELLO" && data->LastAddonRequest && getMSTimeDiff(data->LastAddonRequest, now) < ADDON_REQUEST_COOLDOWN)
        {
            SendStateToAddon(player);
            return false;
        }
        if (request != "HELLO")
            data->LastAddonRequest = now ? now : 1;

        if (request == "ON")
            player->RemoveFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);
        else if (request == "OFF")
            player->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);
        else if (request == "DEFAULT")
        {
            if (!xpOff && !data->FollowsDefault) // .xp default always deletes, to repair a stray row
                SetPlayerXpRate(player, individualXp.DefaultRate, true);
        }
        else if (request.substr(0, 4) == "SET:")
        {
            // A rate the command would turn down is ignored; the R below puts the setting back.
            Optional<float> const rate = Acore::StringTo<float>(request.substr(4));
            if (!xpOff && rate && *rate >= MIN_RATE && *rate <= GetMaxXpRate(player) && *rate != GetPlayerXpRate(player))
                SetPlayerXpRate(player, *rate);
        }

        SendStateToAddon(player);
        return false;
    }
};

class IndividualXPCommand : public CommandScript
{
public:
    IndividualXPCommand() : CommandScript("IndividualXPCommand") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable IndividualXPCommandTable =
        {
            { "enable",  HandleEnableCommand, SEC_PLAYER, Console::No },
            { "disable",  HandleDisableCommand, SEC_PLAYER, Console::No },
            { "view",  HandleViewCommand, SEC_PLAYER, Console::No },
            { "set",  HandleSetCommand, SEC_PLAYER, Console::No },
            { "default",  HandleDefaultCommand, SEC_PLAYER, Console::No }
        };

        static ChatCommandTable IndividualXPBaseTable =
        {
            { "xp",  IndividualXPCommandTable }
        };

        return IndividualXPBaseTable;
    }

    static bool HandleViewCommand(ChatHandler* handler)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
        {
            handler->PSendSysMessage(ACORE_STRING_RATES_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }
        else
        {
            handler->PSendSysMessage(ACORE_STRING_COMMAND_VIEW, GetHeldXpRate(player));
            SendRampLine(handler, player, GetHeldXpRate(player));
        }
        return true;
    }

    static bool HandleSetCommand(ChatHandler* handler, float rate)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!rate)
            return false;

        Player* player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
        {
            handler->PSendSysMessage(ACORE_STRING_RATES_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }
        else
        {
            if (rate > GetMaxXpRate(player))
            {
                handler->PSendSysMessage(ACORE_STRING_MAX_RATE, GetMaxXpRate(player));
                handler->SetSentErrorMessage(true);
                return false;
            }

            if (rate < MIN_RATE)
            {
                handler->PSendSysMessage(ACORE_STRING_MIN_RATE);
                handler->SetSentErrorMessage(true);
                return false;
            }

            SetPlayerXpRate(player, rate);
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_SET, rate);
            SendRampLine(handler, player, rate);
            SendStateToAddon(player);
            return true;
        }
    }

    static bool HandleDisableCommand(ChatHandler* handler)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (!player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
        {
            // Turn Disabled On But Don't Change Value...
            player->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_DISABLED);
            SendStateToAddon(player);
            return true;
        }
        else
        {
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_RATES_DISABLED);
            return false;
        }
    }

    static bool HandleEnableCommand(ChatHandler* handler)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
        {
            player->RemoveFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_ENABLED);
            SendStateToAddon(player);
        }
        else
        {
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_RATES_DISABLED);
        }

        return true;
    }

    static bool HandleDefaultCommand(ChatHandler* handler)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
        {
            handler->PSendSysMessage(ACORE_STRING_RATES_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }
        else
        {
            SetPlayerXpRate(player, individualXp.DefaultRate, true);
            float const held = GetHeldXpRate(player);
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_DEFAULT, held);
            SendRampLine(handler, player, held);
            SendStateToAddon(player);
            return true;
        }
    }
};

void AddIndividualXPScripts()
{
    new IndividualXPConf();
    new IndividualXP();
    new IndividualXPCommand();
}
