#include <cinttypes>
#include "uidsf.hpp"
#include "player.hpp"
#include "message.hpp"
#include "actorpod.hpp"
#include "server.hpp"
#include "dbcomid.hpp"
#include "serverargparser.hpp"

extern ServerArgParser *g_serverArgParser;
corof::awaitable<> Player::net_CM_ACTION(uint8_t, const uint8_t *pBuf, size_t, uint64_t)
{
    CMAction cmA;
    std::memcpy(&cmA, pBuf, sizeof(cmA));

    if(true
            && cmA.UID == UID()
            && cmA.mapUID == mapUID()){

        // don't do dispatchAction(cmA.action) here
        // we may need to filter some actions to anti-cheat, dispatch in onCMActionXXXX(cmA) if legeal

        switch(to_d(cmA.action.type)){
            case ACTION_STAND   : return onCMActionStand   (cmA);
            case ACTION_MOVE    : return onCMActionMove    (cmA);
            case ACTION_MINE    : return onCMActionMine    (cmA);
            case ACTION_ATTACK  : return onCMActionAttack  (cmA);
            case ACTION_SPELL   : return onCMActionSpell   (cmA);
            case ACTION_PICKUP  : return onCMActionPickUp  (cmA);
            case ACTION_SPINKICK: return onCMActionSpinKick(cmA);
            default             : break;
        }
    }
    return {};
}

corof::awaitable<> Player::net_CM_QUERYCORECORD(uint8_t, const uint8_t *pBuf, size_t, uint64_t)
{
    CMQueryCORecord stCMQCOR;
    std::memcpy(&stCMQCOR, pBuf, sizeof(stCMQCOR));

    if(true
            && stCMQCOR.AimUID
            && stCMQCOR.AimUID != UID()){

        AMQueryCORecord amQCOR;
        std::memset(&amQCOR, 0, sizeof(amQCOR));

        // target UID can ignore it
        // send the query without response requirement

        amQCOR.UID = UID();
        if(!m_actorPod->post(stCMQCOR.AimUID, {AM_QUERYCORECORD, amQCOR})){
            reportDeadUID(stCMQCOR.AimUID);
        }
    }
    return {};
}

corof::awaitable<> Player::net_CM_REQUESTRETRIEVESECUREDITEM(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmRRSI = ClientMsg::conv<CMRequestRetrieveSecuredItem>(buf);
    removeSecuredItem(cmRRSI.itemID, cmRRSI.seqID);
    return {};
}

corof::awaitable<> Player::net_CM_REQUESTSPACEMOVE(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmRSM = ClientMsg::conv<CMRequestSpaceMove>(buf);
    if(cmRSM.mapUID == mapUID()){
        if(co_await requestSpaceMove(cmRSM.X, cmRSM.Y, false)){
            dbUpdateMapGLoc();
        }
    }
    else{
        if(co_await requestMapSwitch(cmRSM.mapUID, cmRSM.X, cmRSM.Y, false)){
            dbUpdateMapGLoc();
        }
    }
}

corof::awaitable<> Player::net_CM_REQUESTMAGICDAMAGE(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmRMD = ClientMsg::conv<CMRequestMagicDamage>(buf);
    dispatchAttackDamage(cmRMD.aimUID, DBCOM_MAGICID(u8"物理攻击"), 0);
    return {};
}

corof::awaitable<> Player::net_CM_REQUESTADDEXP(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmRAE = ClientMsg::conv<CMRequestAddExp>(buf);
    gainExp(to_d(cmRAE.addExp));
    return {};
}

corof::awaitable<> Player::net_CM_REQUESTKILLPETS(uint8_t, const uint8_t *, size_t, uint64_t)
{
    RequestKillPets();
    return {};
}

corof::awaitable<> Player::net_CM_PICKUP(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmPU = ClientMsg::conv<CMPickUp>(buf);
    if(cmPU.mapUID != mapUID()){
        co_return;
    }

    if(!(cmPU.x == X() && cmPU.y == Y())){
        reportStand();
        co_return;
    }

    const auto fnPostPickUpError = [this](uint32_t itemID)
    {
        SMPickUpError smPUE;
        std::memset(&smPUE, 0, sizeof(smPUE));
        smPUE.failedItemID = itemID;
        postNetMessage(SM_PICKUPERROR, smPUE);
    };

    if(m_pickUpLock){
        fnPostPickUpError(0);
        co_return;
    }

    AMPickUp amPU;
    std::memset(&amPU, 0, sizeof(amPU));
    amPU.x = cmPU.x;
    amPU.y = cmPU.y;
    amPU.availableWeight = 500;

    m_pickUpLock = true;
    const auto pickUpLockSg = sgf::guard([this]() noexcept { m_pickUpLock = false; });

    switch(const auto mpk = co_await m_actorPod->send(mapUID(), {AM_PICKUP, amPU}); mpk.type()){
        case AM_PICKUPITEMLIST:
            {
                auto sdPUIL = cerealf::deserialize<SDPickUpItemList>(mpk.data(), mpk.size());
                for(auto &item: sdPUIL.itemList){
                    fflassert(item);
                    const auto &ir = DBCOM_ITEMRECORD(item.itemID);

                    fflassert(ir);
                    if(item.isGold()){
                        setGold(m_sdItemStorage.gold + item.count);
                    }
                    else{
                        addInventoryItem(std::move(item), false);
                    }
                }

                if(sdPUIL.failedItemID){
                    fnPostPickUpError(sdPUIL.failedItemID);
                }
                break;
            }
        default:
            {
                break;
            }
    }
}

corof::awaitable<> Player::net_CM_PING(uint8_t, const uint8_t *pBuf, size_t, uint64_t)
{
    SMPing smP;
    std::memset(&smP, 0, sizeof(smP));
    smP.Tick = ((CMPing *)(pBuf))->Tick; // strict-aliasing issue
    postNetMessage(SM_PING, smP);
    return {};
}

corof::awaitable<> Player::net_CM_QUERYMAPBASEUID(uint8_t, const uint8_t *buf, size_t size, uint64_t respID)
{
    const auto cmQMBUID = ClientMsg::conv<CMQueryMapBaseUID>(buf, size);
    if(DBCOM_MAPRECORD(cmQMBUID.mapID)){
        SMUID smUID;
        std::memset(&smUID, 0, sizeof(smUID));

        smUID.uid = uidsf::getMapBaseUID(cmQMBUID.mapID);
        postNetMessage(SM_UID, smUID, respID);
    }
    else{
        postNetMessage(SM_ERROR, respID);
    }
    return {};
}

corof::awaitable<> Player::net_CM_QUERYGOLD(uint8_t, const uint8_t *, size_t, uint64_t)
{
    reportGold();
    return {};
}

corof::awaitable<> Player::net_CM_NPCEVENT(uint8_t, const uint8_t *buf, size_t bufLen, uint64_t)
{
    const auto cmNPCE = ClientMsg::conv<CMNPCEvent>(buf, bufLen);
    m_actorPod->post(cmNPCE.uid, {AM_NPCEVENT, cerealf::serialize(SDNPCEvent
    {
        .x = X(),
        .y = Y(),
        .mapUID = mapUID(),

        .path = cmNPCE.path,
        .event = cmNPCE.event,
        .value = [&cmNPCE]() -> std::optional<std::string>
        {
            if(cmNPCE.valueSize >= 0){
                return std::string(cmNPCE.value, cmNPCE.valueSize);
            }
            return {};
        }(),
    })});
    return {};
}

corof::awaitable<> Player::net_CM_QUERYSELLITEMLIST(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmQSIL = ClientMsg::conv<CMQuerySellItemList>(buf);
    AMQuerySellItemList amQSIL;

    std::memset(&amQSIL, 0, sizeof(amQSIL));
    amQSIL.itemID = cmQSIL.itemID;
    m_actorPod->post(cmQSIL.npcUID, {AM_QUERYSELLITEMLIST, amQSIL});
    return {};
}

corof::awaitable<> Player::net_CM_QUERYUIDBUFF(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmQUIDB = ClientMsg::conv<CMQueryUIDBuff>(buf);
    if(cmQUIDB.uid == UID()){
        postNetMessage(SM_BUFFIDLIST, cerealf::serialize(SDBuffIDList
        {
            .uid = UID(),
            .idList = m_buffList.getIDList(),
        }));
    }
    else{
        switch(uidf::getUIDType(cmQUIDB.uid)){
            case UID_PLY:
            case UID_MON:
                {
                    m_actorPod->post(cmQUIDB.uid, AM_QUERYUIDBUFF);
                    break;
                }
            default:
                {
                    throw fflerror("invalid uid: %llu, type: %s", to_llu(cmQUIDB.uid), uidf::getUIDTypeCStr(cmQUIDB.uid));
                }
        }
    }
    return {};
}

corof::awaitable<> Player::net_CM_QUERYPLAYERNAME(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmQPN = ClientMsg::conv<CMQueryPlayerName>(buf);
    if(cmQPN.uid == UID()){
        postNetMessage(SM_PLAYERNAME, cerealf::serialize(SDPlayerName
        {
            .uid = UID(),
            .name = name(),
            .nameColor = nameColor(),
        }));
    }
    else if(uidf::isPlayer(cmQPN.uid)){
        m_actorPod->post(cmQPN.uid, AM_QUERYPLAYERNAME);
    }
    return {};
}

corof::awaitable<> Player::net_CM_QUERYPLAYERWLDESP(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmQPWLD = ClientMsg::conv<CMQueryPlayerWLDesp>(buf);
    if(cmQPWLD.uid == UID()){
        postNetMessage(SM_PLAYERWLDESP, cerealf::serialize(SDUIDWLDesp
        {
            .uid = UID(),
            .desp
            {
                .wear = m_sdItemStorage.wear,
                .hair = m_hair,
                .hairColor = m_hairColor,
            },
        }, true));
    }
    else if(uidf::getUIDType(cmQPWLD.uid) == UID_PLY){
        m_actorPod->post(cmQPWLD.uid, AM_QUERYPLAYERWLDESP);
    }
    else{
        throw fflerror("invalid uid: %llu, type: %s", to_llu(cmQPWLD.uid), uidf::getUIDTypeCStr(cmQPWLD.uid));
    }
    return {};
}

corof::awaitable<> Player::net_CM_QUERYCHATPEERLIST(uint8_t, const uint8_t *buf, size_t, uint64_t respID)
{
    const auto cmQPCL = ClientMsg::conv<CMQueryChatPeerList>(buf);
    const auto input = cmQPCL.input.to_str();

    if(input.empty()){
        postNetMessage(SM_ERROR, respID);
    }
    else{
        postNetMessage(SM_OK, cerealf::serialize(dbQueryChatPeerList(input, true, true)), respID);
    }
    return {};
}

corof::awaitable<> Player::net_CM_QUERYCHATMESSAGE(uint8_t, const uint8_t *buf, size_t, uint64_t respID)
{
    const auto cmQCM = ClientMsg::conv<CMQueryChatMessage>(buf);
    if(auto msgOpt = dbQueryChatMessage(cmQCM.msgid); msgOpt.has_value()){
        postNetMessage(SM_OK, cerealf::serialize(msgOpt.value()), respID);
    }
    else{
        postNetMessage(SM_ERROR, respID);
    }
    return {};
}

corof::awaitable<> Player::net_CM_CHATMESSAGE(uint8_t, const uint8_t *buf, size_t bufSize, uint64_t respID)
{
    fflassert(bufSize >= sizeof(CMChatMessageHeader), bufSize);

    CMChatMessageHeader cmCMH;
    std::memcpy(&cmCMH, buf, sizeof(CMChatMessageHeader));

    const SDChatPeerID toCPID(cmCMH.toCPID);
    const auto refIDOpt = cmCMH.hasRef ? std::optional<uint64_t>(to_u64(cmCMH.refID)) : std::nullopt;

    std::string msgBuf;
    msgBuf = as_sv(buf + sizeof(CMChatMessageHeader), bufSize - sizeof(CMChatMessageHeader));

    const auto [msgId, tstamp] = dbSaveChatMessage(cpid(), toCPID, msgBuf, refIDOpt);
    const auto fnForwardChatMessage = [toCPID, refIDOpt, msgBuf, msgId, tstamp, this](uint32_t playerDBID)
    {
        forwardNetPackage(uidf::getPlayerUID(playerDBID), SM_CHATMESSAGELIST, cerealf::serialize(SDChatMessageList
        {
            SDChatMessage
            {
                .seq = SDChatMessageDBSeq
                {
                    .id = msgId,
                    .timestamp = tstamp,
                },

                .refer = refIDOpt,

                .from = cpid(),
                .to   = toCPID,

                .message = msgBuf, // keep serialized
            },
        }));
    };

    switch(toCPID.type()){
        case CP_SPECIAL:
            {
                break;
            }
        case CP_PLAYER:
            {
                if(toCPID != cpid()){
                    fnForwardChatMessage(toCPID.id());
                }
                break;
            }
        case CP_GROUP:
            {
                for(const auto memberDBID: dbLoadChatGroupMemberList(toCPID.id())){
                    if(memberDBID != dbid()){
                        fnForwardChatMessage(memberDBID);
                    }
                }
                break;
            }
        default:
            {
                throw fflvalue(toCPID.asU64());
            }
    }

    postNetMessage(SM_OK, cerealf::serialize(SDChatMessageDBSeq
    {
        .id = msgId,
        .timestamp = tstamp,

    }), respID);

    return {};
}

corof::awaitable<> Player::net_CM_ADDFRIEND(uint8_t, const uint8_t *buf, size_t, uint64_t respID)
{
    const auto fnPostNetMessage = [respID, this](int notif)
    {
        postNetMessage(SM_OK, cerealf::serialize(SDAddFriendNotif
        {
            .notif = notif,
        }),

        respID);
    };

    const auto cmAF = ClientMsg::conv<CMAddFriend>(buf);
    const SDChatPeerID sdCPID(cmAF.cpid);

    if(sdCPID == cpid()){
        fnPostNetMessage(AF_INVALID);
        return {};
    }

    if(!dbHasPlayer(sdCPID.id())){
        fnPostNetMessage(AF_INVALID);
        return {};
    }

    if(findFriendChatPeer(sdCPID)){
        fnPostNetMessage(AF_EXIST);
        return {};
    }

    if(dbIsBlocked(sdCPID.id(), dbid())){
        fnPostNetMessage(AF_BLOCKED);
        return {};
    }

    const auto fnForwardSystemMessage = [&sdCPID, this](const std::string xmlChatMsg)
    {
        const auto msgBuf = cerealf::serialize(xmlChatMsg);
        const auto [msgId, tstamp] = dbSaveChatMessage(SDChatPeerID(CP_SPECIAL, SYS_CHATDBID_SYSTEM), sdCPID, msgBuf, {});

        forwardNetPackage(uidf::getPlayerUID(sdCPID.id()), SM_CHATMESSAGELIST, cerealf::serialize(SDChatMessageList
        {
            SDChatMessage
            {
                .seq = SDChatMessageDBSeq
                {
                    .id = msgId,
                    .timestamp = tstamp,
                },

                .refer = std::nullopt,

                .from {CP_SPECIAL, SYS_CHATDBID_SYSTEM},
                .to   {CP_PLAYER, sdCPID.id()},

                .message = msgBuf, // keep serialized
            },
        }));
    };

    switch(SDRuntimeConfig_getConfig<RTCFG_好友申请>(dbGetRuntimeConfig(sdCPID.id()))){
        case FR_ACCEPT:
            {
                const auto notif = dbAddFriend(dbid(), sdCPID.id());
                fnPostNetMessage(notif);

                if(notif == AF_ACCEPTED){
                    if(auto sdCPOpt = dbLoadChatPeer(sdCPID.asU64()); sdCPOpt.has_value() && !findFriendChatPeer(sdCPID)){
                        m_sdFriendList.push_back(std::move(sdCPOpt.value()));
                    }
                    fnForwardSystemMessage(str_printf(R"###(<layout><par>%s已经添加你为好友。</par></layout>)###", to_cstr(m_name)));
                }
                return {};
            }
        case FR_REJECT:
            {
                fnPostNetMessage(AF_REJECTED);
                return {};
            }
        default:
            {
                fnPostNetMessage(AF_PENDING);
                if(dbIsFriend(sdCPID.id(), dbid())){
                    fnForwardSystemMessage(str_printf(R"###(
                    <layout>
                        <par><t color="red">%s</t>申请添加你为好友，你可以选择</par>
                        <par><event id="%s" accept="" cpid="%llu"             >同意</event></par>
                        <par><event id="%s" reject="" cpid="%llu"             >拒绝</event></par>
                        <par><event id="%s" reject="" cpid="%llu"     block="">拒绝并将对方加入黑名单</event></par>
                    </layout>
                    )###",

                    to_cstr(m_name),
                    SYS_AFRESP, to_llu(cpid().asU64()),
                    SYS_AFRESP, to_llu(cpid().asU64()),
                    SYS_AFRESP, to_llu(cpid().asU64())));
                }
                else{
                    fnForwardSystemMessage(str_printf(R"###(
                    <layout>
                        <par><t color="red">%s</t>申请添加你为好友，你可以选择</par>
                        <par><event id="%s" accept="" cpid="%llu"             >同意</event></par>
                        <par><event id="%s" accept="" cpid="%llu" addfriend="">同意并添加对方为好友</event></par>
                        <par><event id="%s" reject="" cpid="%llu"             >拒绝</event></par>
                        <par><event id="%s" reject="" cpid="%llu"     block="">拒绝并将对方加入黑名单</event></par>
                    </layout>
                    )###",

                    to_cstr(m_name),
                    SYS_AFRESP, to_llu(cpid().asU64()),
                    SYS_AFRESP, to_llu(cpid().asU64()),
                    SYS_AFRESP, to_llu(cpid().asU64()),
                    SYS_AFRESP, to_llu(cpid().asU64())));
                }
                return {};
            }
    }
}

corof::awaitable<> Player::net_CM_ACCEPTADDFRIEND(uint8_t, const uint8_t *buf, size_t, uint64_t respID)
{
    const auto cmAAF = ClientMsg::conv<CMAcceptAddFriend>(buf);
    const SDChatPeerID sdCPID(cmAAF.cpid);

    if(sdCPID == cpid()){
        postNetMessage(SM_ERROR, respID);
        return {};
    }

    if(!dbHasPlayer(sdCPID.id())){
        postNetMessage(SM_ERROR, respID);
        return {};
    }

    const auto notif = dbAddFriend(sdCPID.id(), dbid());
    postNetMessage(SM_OK, respID);

    if(notif == AF_ACCEPTED){
        forwardNetPackage(uidf::getPlayerUID(sdCPID.id()), SM_ADDFRIENDACCEPTED, cerealf::serialize(dbLoadChatPeer(cpid().asU64()).value()));
    }

    return {};
}

corof::awaitable<> Player::net_CM_REJECTADDFRIEND(uint8_t, const uint8_t *buf, size_t, uint64_t respID)
{
    const auto cmRAF = ClientMsg::conv<CMRejectAddFriend>(buf);
    const SDChatPeerID sdCPID(cmRAF.cpid);

    if(sdCPID == cpid()){
        postNetMessage(SM_ERROR, respID);
        return {};
    }

    if(!dbHasPlayer(sdCPID.id())){
        postNetMessage(SM_ERROR, respID);
        return {};
    }

    postNetMessage(SM_OK, respID);
    forwardNetPackage(uidf::getPlayerUID(sdCPID.id()), SM_ADDFRIENDREJECTED, cerealf::serialize(dbLoadChatPeer(cpid().asU64()).value()));
    return {};
}

corof::awaitable<> Player::net_CM_BLOCKPLAYER(uint8_t, const uint8_t *buf, size_t, uint64_t respID)
{
    const auto cmBP = ClientMsg::conv<CMBlockPlayer>(buf);
    const SDChatPeerID sdCPID(cmBP.cpid);

    if(sdCPID == cpid()){
        postNetMessage(SM_ERROR, respID);
        return {};
    }

    if(!dbHasPlayer(sdCPID.id())){
        postNetMessage(SM_ERROR, respID);
        return {};
    }

    dbBlockPlayer(dbid(), sdCPID.id());
    postNetMessage(SM_OK, respID);
    return {};
}

corof::awaitable<> Player::net_CM_BUY(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmB = ClientMsg::conv<CMBuy>(buf);
    if(uidf::getUIDType(cmB.npcUID) != UID_NPC){
        throw fflerror("invalid uid: %llu, type: %s", to_llu(cmB.npcUID), uidf::getUIDTypeCStr(cmB.npcUID));
    }

    AMBuy amB;
    std::memset(&amB, 0, sizeof(amB));

    amB.itemID = cmB.itemID;
    amB.seqID  = cmB.seqID;
    amB.count  = cmB.count;

    const auto mpk = co_await m_actorPod->send(cmB.npcUID, {AM_BUY, amB});

    const auto fnPostBuyError = [&cmB, &mpk, this](int buyError)
    {
        SMBuyError smBE;
        std::memset(&smBE, 0, sizeof(smBE));

        smBE.npcUID = mpk.from();
        smBE.itemID = cmB.itemID;
        smBE. seqID = cmB. seqID;
        smBE. error =   buyError;
        postNetMessage(SM_BUYERROR, smBE);
    };

    switch(mpk.type()){
        case AM_BUYCOST:
            {
                uint32_t lackItemID = 0;
                const auto sdBC = cerealf::deserialize<SDBuyCost>(mpk.data(), mpk.size());

                if(cmB.itemID != sdBC.item.itemID || cmB.seqID != sdBC.item.seqID){
                    throw fflerror("item asked and sold are not same: buyItemID = %llu, buySeqID = %llu, soldItemID = %llu, soldSeqID = %llu", to_llu(cmB.itemID), to_llu(cmB.seqID), to_llu(sdBC.item.itemID), to_llu(sdBC.item.seqID));
                }

                for(const auto &costItem: sdBC.costList){
                    if(costItem.isGold()){
                        if(m_sdItemStorage.gold < costItem.count){
                            lackItemID = costItem.itemID;
                            break;
                        }
                    }
                    else if(!hasInventoryItem(costItem.itemID, 0, costItem.count)){
                        lackItemID = costItem.itemID;
                        break;
                    }
                }

                if(lackItemID){
                    m_actorPod->post(mpk.fromAddr(), AM_ERROR);
                    fnPostBuyError(BUYERR_INSUFFCIENT);
                }
                else{
                    for(const auto &costItem: sdBC.costList){
                        if(costItem.isGold()){
                            setGold(m_sdItemStorage.gold - costItem.count);
                        }
                        else{
                            removeInventoryItem(costItem.itemID, 0, costItem.count);
                        }
                    }

                    const auto &ir = DBCOM_ITEMRECORD(sdBC.item.itemID);
                    if(!ir){
                        throw fflerror("bad item: itemID = %llu", to_llu(sdBC.item.itemID));
                    }

                    if(ir.packable()){
                        size_t doneCount = 0;
                        while(doneCount < sdBC.item.count){
                            const auto currCount = std::min<size_t>(SYS_INVGRIDMAXHOLD, sdBC.item.count - doneCount);
                            doneCount += addInventoryItem(SDItem
                            {
                                .itemID = sdBC.item.itemID,
                                .count = currCount,
                            }, false).count;
                        }
                    }
                    else{
                        addInventoryItem(sdBC.item, false);
                    }
                    m_actorPod->post(mpk.fromAddr(), AM_OK);

                    if(!ir.packable()){
                        SMBuySucceed smBS;
                        std::memset(&smBS, 0, sizeof(smBS));

                        smBS.npcUID = mpk.from();
                        smBS.itemID = sdBC.item.itemID;
                        smBS. seqID = sdBC.item.seqID;
                        postNetMessage(SM_BUYSUCCEED, smBS);
                    }
                }
                break;
            }
        case AM_BUYERROR:
            {
                SMBuyError smBE;
                std::memset(&smBE, 0, sizeof(smBE));

                smBE.npcUID = cmB.npcUID;
                smBE.itemID = cmB.itemID;
                smBE. seqID = cmB. seqID;
                smBE. error = mpk.conv<AMBuyError>().error;
                postNetMessage(SM_BUYERROR, smBE);
                break;
            }
        default:
            {
                throw fflvalue(mpkName(mpk.type()));
            }
    }
}

corof::awaitable<> Player::net_CM_REQUESTEQUIPWEAR(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmREW = ClientMsg::conv<CMRequestEquipWear>(buf);
    const auto fnPostEquipError = [&cmREW, this](int equipError)
    {
        SMEquipWearError smEWE;
        std::memset(&smEWE, 0, sizeof(smEWE));

        smEWE.itemID = cmREW.itemID;
        smEWE.seqID = cmREW.seqID;
        smEWE.error = equipError;
        postNetMessage(SM_EQUIPWEARERROR, smEWE);
    };

    const auto &ir = DBCOM_ITEMRECORD(cmREW.itemID);
    if(!ir){
        fnPostEquipError(EQWERR_BADITEM);
        return {};
    }

    const auto wltype = to_d(cmREW.wltype);
    if(!(wltype >= WLG_BEGIN && wltype < WLG_END)){
        fnPostEquipError(EQWERR_BADWLTYPE);
        return {};
    }

    if(!ir.wearable(wltype)){
        fnPostEquipError(EQWERR_BADWLTYPE);
        return {};
    }

    const auto item = findInventoryItem(cmREW.itemID, cmREW.seqID);
    if(!item){
        fnPostEquipError(EQWERR_NOITEM);
        return {};
    }

    if(!canWear(cmREW.itemID, wltype)){
        fnPostEquipError(EQWERR_INSUFF);
        return {};
    }

    const auto currItem = m_sdItemStorage.wear.getWLItem(wltype);
    setWLItem(cmREW.wltype, item);

    removeInventoryItem(item);
    dbUpdateWearItem(wltype, item);
    postNetMessage(SM_EQUIPWEAR, cerealf::serialize(SDEquipWear
    {
        .uid = UID(),
        .wltype = wltype,
        .item = item,
    }));

    // put last item into inventory
    // should I support to set it as grabbed?
    // when user finishes item switch, they usually directly put it into inventory

    if(currItem){
        addInventoryItem(currItem, false);
    }

    if(const auto buffIDOpt = item.getExtAttr<SDItem::EA_BUFFID_t>(); buffIDOpt.has_value() && buffIDOpt.value()){
        if(const auto pbuff = addBuff(UID(), 0, buffIDOpt.value())){
            addWLOffTrigger(wltype, [buffSeq = pbuff->buffSeq(), this]()
            {
                removeBuff(buffSeq, true);
            });
        }
    }

    return {};
}

corof::awaitable<> Player::net_CM_REQUESTEQUIPBELT(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmREB = ClientMsg::conv<CMRequestEquipBelt>(buf);
    const auto fnPostEquipError = [&cmREB, this](int equipError)
    {
        SMEquipBeltError smEBE;
        std::memset(&smEBE, 0, sizeof(smEBE));

        smEBE.itemID = cmREB.itemID;
        smEBE.seqID = cmREB.seqID;
        smEBE.error = equipError;
        postNetMessage(SM_EQUIPBELTERROR, smEBE);
    };

    const auto &ir = DBCOM_ITEMRECORD(cmREB.itemID);
    if(!ir){
        fnPostEquipError(EQBERR_BADITEM);
        return {};
    }

    if(!ir.beltable()){
        fnPostEquipError(EQBERR_BADITEMTYPE);
        return {};
    }

    const auto slot = to_d(cmREB.slot);
    if(!(slot >= 0 && slot < 6)){
        fnPostEquipError(EQBERR_BADSLOT);
        return {};
    }

    const auto item = findInventoryItem(cmREB.itemID, cmREB.seqID);
    if(!item){
        fnPostEquipError(EQBERR_NOITEM);
        return {};
    }

    const auto currItem = m_sdItemStorage.belt.list.at(slot);
    m_sdItemStorage.belt.list.at(slot) = item;

    removeInventoryItem(item);
    dbUpdateBeltItem(slot, item);
    postNetMessage(SM_EQUIPBELT, cerealf::serialize(SDEquipBelt
    {
        .slot = slot,
        .item = item,
    }));

    // put last item into inventory
    // should I support to set it as grabbed?
    // when user finishes item switch, they usually directly put it into inventory

    if(currItem){
        addInventoryItem(currItem, false);
    }

    return {};
}

corof::awaitable<> Player::net_CM_REQUESTGRABWEAR(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmRGW = ClientMsg::conv<CMRequestGrabWear>(buf);
    const auto wltype = to_d(cmRGW.wltype);
    const auto fnPostGrabError = [&cmRGW, this](int grabError)
    {
        SMGrabWearError smGWE;
        std::memset(&smGWE, 0, sizeof(smGWE));
        smGWE.error = grabError;
        postNetMessage(SM_GRABWEARERROR, smGWE);
    };

    const auto currItem = m_sdItemStorage.wear.getWLItem(wltype);
    if(!currItem){
        fnPostGrabError(GWERR_NOITEM);
        return {};
    }

    // server doesn not track if item is grabbed or in inventory
    // when disarms the wear item, server always put it into the inventory

    setWLItem(wltype, {});
    dbRemoveWearItem(wltype);

    const auto &addedItem = m_sdItemStorage.inventory.add(currItem, false);
    dbUpdateInventoryItem(addedItem);

    postNetMessage(SM_GRABWEAR, cerealf::serialize(SDGrabWear
    {
        .wltype = wltype,
        .item = addedItem,
    }));

    if(auto cbp = m_onWLOff.find(wltype); cbp != m_onWLOff.end()){
        fflassert(cbp->second);
        cbp->second();
        m_onWLOff.erase(cbp);
    }

    return {};
}

corof::awaitable<> Player::net_CM_REQUESTGRABBELT(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmRGB = ClientMsg::conv<CMRequestGrabBelt>(buf);
    const auto fnPostGrabError = [&cmRGB, this](int grabError)
    {
        SMGrabBeltError smGBE;
        std::memset(&smGBE, 0, sizeof(smGBE));
        smGBE.error = grabError;
        postNetMessage(SM_GRABBELTERROR, smGBE);
    };

    const auto currItem = m_sdItemStorage.belt.list.at(cmRGB.slot);
    if(!currItem){
        fnPostGrabError(GBERR_NOITEM);
        return {};
    }

    // server doesn not track if item is grabbed or in inventory
    // when disarms the wear item, server always put it into the inventory

    m_sdItemStorage.belt.list.at(cmRGB.slot) = {};
    dbRemoveBeltItem(cmRGB.slot);

    const auto &addedItem = m_sdItemStorage.inventory.add(currItem, false);
    dbUpdateInventoryItem(addedItem);

    postNetMessage(SM_GRABBELT, cerealf::serialize(SDGrabBelt
    {
        .slot = to_d(cmRGB.slot),
        .item = addedItem,
    }));

    return {};
}

corof::awaitable<> Player::net_CM_REQUESTJOINTEAM(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmRJT = ClientMsg::conv<CMRequestJoinTeam>(buf);
    if(!uidf::isPlayer(cmRJT.uid)){
        co_return;
    }

    if(cmRJT.uid == UID()){
        if(m_teamLeader){
            SMTeamError smTE;
            std::memset(&smTE, 0, sizeof(smTE));
            smTE.error = TEAMERR_INTEAM;
            postNetMessage(SM_TEAMERROR, smTE);
        }
        else{
            m_teamLeader = UID();
            m_teamMemberList = std::vector<uint64_t>{UID()};
            co_await reportTeamMemberList();
        }
    }
    else if(m_teamLeader == UID()){
        m_teamMemberList.push_back(cmRJT.uid);
        m_actorPod->post(cmRJT.uid, AM_TEAMUPDATE);
        co_await reportTeamMemberList();
    }
    else{
        m_actorPod->post(cmRJT.uid, {AM_REQUESTJOINTEAM, cerealf::serialize(SDRequestJoinTeam
        {
            .player
            {
                .uid = UID(),
                .level = level(),
                .name = name(),
            },
        })});
    }
}

corof::awaitable<> Player::net_CM_REQUESTLEAVETEAM(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmRLT = ClientMsg::conv<CMRequestLeaveTeam>(buf);
    if(!uidf::isPlayer(cmRLT.uid)){
        co_return;
    }

    if(!m_teamLeader){
        co_return;
    }

    if(m_teamLeader == UID()){
        for(const auto member: m_teamMemberList){
            if(member != UID()){
                m_actorPod->post(member, AM_TEAMUPDATE);
            }
        }

        if(cmRLT.uid == UID()){
            m_teamLeader = 0;
            m_teamMemberList.clear();
        }
        else{
            m_teamMemberList.erase(std::remove(m_teamMemberList.begin(), m_teamMemberList.end(), cmRLT.uid), m_teamMemberList.end());
        }

        co_await reportTeamMemberList();
    }
    else if(cmRLT.uid == UID()){
        m_actorPod->post(m_teamLeader, AM_REQUESTLEAVETEAM);
    }
}

corof::awaitable<> Player::net_CM_DROPITEM(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmDI = ClientMsg::conv<CMDropItem>(buf);
    auto dropItem = [&cmDI, this]() -> SDItem
    {
        if(const auto &singleItem = findInventoryItem(cmDI.itemID, cmDI.seqID)){
            return singleItem;
        }

        if(hasInventoryItem(cmDI.itemID, cmDI.seqID, cmDI.count)){
            return SDItem
            {
                .itemID = cmDI.itemID,
                .seqID = cmDI.seqID,
                .count = to_uz(cmDI.count),
            };
        }

        return {};
    }();

    fflassert(dropItem);
    removeInventoryItem(dropItem);

    m_actorPod->post(mapUID(), {AM_DROPITEM, cerealf::serialize(SDDropItem
    {
        .x = X(),
        .y = Y(),
        .item = std::move(dropItem),
    })});

    return {};
}

corof::awaitable<> Player::net_CM_CONSUMEITEM(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmCI = ClientMsg::conv<CMConsumeItem>(buf);
    fflassert((SDItem
    {
        .itemID = cmCI.itemID,
        .seqID = cmCI.seqID,
        .count = to_uz(cmCI.count),
    }));

    const auto &ir = DBCOM_ITEMRECORD(cmCI.itemID);
    fflassert(ir);

    bool consumed = false;
    if(ir.isBook()){
        consumed = consumeBook(cmCI.itemID);
    }
    else if(ir.isPotion()){
        consumed = consumePotion(cmCI.itemID);
    }
    else{
        // TODO
    }

    if(consumed){
        removeInventoryItem(cmCI.itemID, cmCI.seqID, cmCI.count);
    }

    return {};
}

corof::awaitable<> Player::net_CM_MAKEITEM(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmMI = ClientMsg::conv<CMMakeItem>(buf);
    fflassert(cmMI.count >= 1, cmMI.count);

    const auto &ir = DBCOM_ITEMRECORD(cmMI.itemID);
    fflassert(ir, cmMI.itemID, cmMI.count);

    size_t done = 0;
    while(done < cmMI.count){
        const SDItem item
        {
            .itemID = cmMI.itemID,
            .count = ir.packable() ? std::min<size_t>(SYS_INVGRIDMAXHOLD, cmMI.count - done) : to_uz(1),
        };

        fflassert(item, item.itemID, item.count);
        addInventoryItem(item, false);

        done += item.count;
    }

    return {};
}

corof::awaitable<> Player::net_CM_SETMAGICKEY(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmSMK = ClientMsg::conv<CMSetMagicKey>(buf);
    dbUpdateMagicKey(cmSMK.magicID, cmSMK.key);
    return {};
}

corof::awaitable<> Player::net_CM_SETRUNTIMECONFIG(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmSRC = ClientMsg::conv<CMSetRuntimeConfig>(buf);

    fflassert(cmSRC.type >= RTCFG_BEGIN, cmSRC.type);
    fflassert(cmSRC.type <  RTCFG_END  , cmSRC.type);

    if(m_sdPlayerConfig.runtimeConfig.setConfig(cmSRC.type, cmSRC.buf.as_sv())){
        dbUpdateRuntimeConfig();
    }

    return {};
}

corof::awaitable<> Player::net_CM_REQUESTLATESTCHATMESSAGE(uint8_t, const uint8_t *buf, size_t, uint64_t)
{
    const auto cmRLCM = ClientMsg::conv<CMRequestLatestChatMessage>(buf);
    if(!cmRLCM.cpidList.empty()){
        postNetMessage(SM_CHATMESSAGELIST, cerealf::serialize(dbRetrieveLatestChatMessage(as_span(cmRLCM.cpidList.data, cmRLCM.cpidList.size), cmRLCM.limitCount, cmRLCM.includeSend, cmRLCM.includeRecv)));
    }

    return {};
}

corof::awaitable<> Player::net_CM_CREATECHATGROUP(uint8_t, const uint8_t *buf, size_t, uint64_t respID)
{
    const auto cmCCG = ClientMsg::conv<CMCreateChatGroup>(buf);
    const auto sdBuf = cerealf::serialize(dbCreateChatGroup(cmCCG.name.as_sv().data(), as_span(cmCCG.list.data, cmCCG.list.size)));

    for(const auto memberDBID: as_span<uint32_t>(cmCCG.list.data, cmCCG.list.size)){
        if(memberDBID != dbid()){
            forwardNetPackage(uidf::getPlayerUID(memberDBID), SM_CREATECHATGROUP, sdBuf);
        }
    }

    postNetMessage(SM_OK, sdBuf, respID);
    return {};
}
