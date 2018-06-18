// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2017-2018 The NIX Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeghostnode.h"
#include "darksend.h"
#include "instantx.h"
#include "key.h"
#include "validation.h"
#include "ghostnode-sync.h"
#include "ghostnodeman.h"
#include "net.h"
#include "protocol.h"
#include "spork.h"
#include "sync.h"
#include "txmempool.h"
#include "util.h"
#include "consensus/validation.h"

#include <boost/algorithm/string/replace.hpp>
#include <boost/thread.hpp>

extern CWallet* pwalletMain;
extern CTxMemPool mempool;

bool fEnableInstantSend = true;
int nInstantSendDepth = DEFAULT_INSTANTSEND_DEPTH;
int nCompleteTXLocks;

CInstantSend instantsend;

// Transaction Locks
//
// step 1) Some node announces intention to lock transaction inputs via "txlreg" message
// step 2) Top COutPointLock::SIGNATURES_TOTAL ghostnodes per each spent outpoint push "txvote" message
// step 3) Once there are COutPointLock::SIGNATURES_REQUIRED valid "txvote" messages per each spent outpoint
//         for a corresponding "txlreg" message, all outpoints from that tx are treated as locked

//
// CInstantSend
//

void CInstantSend::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if(fLiteMode) return; // disable all Dash specific functionality
//    if(!sporkManager.IsSporkActive(SPORK_2_INSTANTSEND_ENABLED)) return;

    // Ignore any InstantSend messages until ghostnode list is synced
    if(!ghostnodeSync.IsGhostnodeListSynced()) return;

    // NOTE: NetMsgType::TXLOCKREQUEST is handled via ProcessMessage() in main.cpp

//    if (strCommand == NetMsgType::TXLOCKVOTE) // InstantSend Transaction Lock Consensus Votes
//    {
//        if(pfrom->nVersion < MIN_INSTANTSEND_PROTO_VERSION) return;
//
//        CTxLockVote vote;
//        vRecv >> vote;
//
//        LOCK2(cs_main, cs_instantsend);
//
//        uint256 nVoteHash = vote.GetHash();
//
//        if(mapTxLockVotes.count(nVoteHash)) return;
//        mapTxLockVotes.insert(std::make_pair(nVoteHash, vote));
//
//        ProcessTxLockVote(pfrom, vote);
//
//        return;
//    }
}

bool CInstantSend::ProcessTxLockRequest(const CTxLockRequest& txLockRequest)
{
    LOCK2(cs_main, cs_instantsend);

    uint256 txHash = txLockRequest.GetHash();

    // Check to see if we conflict with existing completed lock,
    // fail if so, there can't be 2 completed locks for the same outpoint
    BOOST_FOREACH(const CTxIn& txin, txLockRequest.vin) {
        std::map<COutPoint, uint256>::iterator it = mapLockedOutpoints.find(txin.prevout);
        if(it != mapLockedOutpoints.end()) {
            // Conflicting with complete lock, ignore this one
            // (this could be the one we have but we don't want to try to lock it twice anyway)
             //LogPrintf("CInstantSend::ProcessTxLockRequest -- WARNING: Found conflicting completed Transaction Lock, skipping current one, txid=%s, completed lock txid=%s\n",
                //    txLockRequest.GetHash().ToString(), it->second.ToString());
            return false;
        }
    }

    // Check to see if there are votes for conflicting request,
    // if so - do not fail, just warn user
    BOOST_FOREACH(const CTxIn& txin, txLockRequest.vin) {
        std::map<COutPoint, std::set<uint256> >::iterator it = mapVotedOutpoints.find(txin.prevout);
        if(it != mapVotedOutpoints.end()) {
            BOOST_FOREACH(const uint256& hash, it->second) {
                if(hash != txLockRequest.GetHash()) {
                     //LogPrintf("instantsend", "CInstantSend::ProcessTxLockRequest -- Double spend attempt! %s\n", txin.prevout.ToStringShort());
                    // do not fail here, let it go and see which one will get the votes to be locked
                }
            }
        }
    }

    if(!CreateTxLockCandidate(txLockRequest)) {
        // smth is not right
         //LogPrintf("CInstantSend::ProcessTxLockRequest -- CreateTxLockCandidate failed, txid=%s\n", txHash.ToString());
        return false;
    }
     //LogPrintf("CInstantSend::ProcessTxLockRequest -- accepted, txid=%s\n", txHash.ToString());

    std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.find(txHash);
    CTxLockCandidate& txLockCandidate = itLockCandidate->second;
    Vote(txLockCandidate);
    ProcessOrphanTxLockVotes();

    // Ghostnodes will sometimes propagate votes before the transaction is known to the client.
    // If this just happened - lock inputs, resolve conflicting locks, update transaction status
    // forcing external script notification.
    TryToFinalizeLockCandidate(txLockCandidate);

    return true;
}

bool CInstantSend::CreateTxLockCandidate(const CTxLockRequest& txLockRequest)
{
    // Normally we should require all outpoints to be unspent, but in case we are reprocessing
    // because of a lot of legit orphan votes we should also check already spent outpoints.
    uint256 txHash = txLockRequest.GetHash();
    if(!txLockRequest.IsValid(!IsEnoughOrphanVotesForTx(txLockRequest))) return false;

    LOCK(cs_instantsend);

    std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.find(txHash);
    if(itLockCandidate == mapTxLockCandidates.end()) {
         //LogPrintf("CInstantSend::CreateTxLockCandidate -- new, txid=%s\n", txHash.ToString());

        CTxLockCandidate txLockCandidate(txLockRequest);
        // all inputs should already be checked by txLockRequest.IsValid() above, just use them now
        BOOST_REVERSE_FOREACH(const CTxIn& txin, txLockRequest.vin) {
            txLockCandidate.AddOutPointLock(txin.prevout);
        }
        mapTxLockCandidates.insert(std::make_pair(txHash, txLockCandidate));
    } else {
         //LogPrintf("instantsend", "CInstantSend::CreateTxLockCandidate -- seen, txid=%s\n", txHash.ToString());
    }

    return true;
}

void CInstantSend::Vote(CTxLockCandidate& txLockCandidate)
{
    if(!fGhostNode) return;

    LOCK2(cs_main, cs_instantsend);

    uint256 txHash = txLockCandidate.GetHash();
    // check if we need to vote on this candidate's outpoints,
    // it's possible that we need to vote for several of them
    std::map<COutPoint, COutPointLock>::iterator itOutpointLock = txLockCandidate.mapOutPointLocks.begin();
    while(itOutpointLock != txLockCandidate.mapOutPointLocks.end()) {

        int nPrevoutHeight = GetUTXOHeight(itOutpointLock->first);
        if(nPrevoutHeight == -1) {
             //LogPrintf("instantsend", "CInstantSend::Vote -- Failed to find UTXO %s\n", itOutpointLock->first.ToStringShort());
            return;
        }

        int nLockInputHeight = nPrevoutHeight + 4;

        int n = mnodeman.GetGhostnodeRank(activeGhostnode.vin, nLockInputHeight, MIN_INSTANTSEND_PROTO_VERSION);

        if(n == -1) {
             //LogPrintf("instantsend", "CInstantSend::Vote -- Unknown Ghostnode %s\n", activeGhostnode.vin.prevout.ToStringShort());
            ++itOutpointLock;
            continue;
        }

        int nSignaturesTotal = COutPointLock::SIGNATURES_TOTAL;
        if(n > nSignaturesTotal) {
             //LogPrintf("instantsend", "CInstantSend::Vote -- Ghostnode not in the top %d (%d)\n", nSignaturesTotal, n);
            ++itOutpointLock;
            continue;
        }

         //LogPrintf("instantsend", "CInstantSend::Vote -- In the top %d (%d)\n", nSignaturesTotal, n);

        std::map<COutPoint, std::set<uint256> >::iterator itVoted = mapVotedOutpoints.find(itOutpointLock->first);

        // Check to see if we already voted for this outpoint,
        // refuse to vote twice or to include the same outpoint in another tx
        bool fAlreadyVoted = false;
        if(itVoted != mapVotedOutpoints.end()) {
            BOOST_FOREACH(const uint256& hash, itVoted->second) {
                std::map<uint256, CTxLockCandidate>::iterator it2 = mapTxLockCandidates.find(hash);
                if(it2->second.HasGhostnodeVoted(itOutpointLock->first, activeGhostnode.vin.prevout)) {
                    // we already voted for this outpoint to be included either in the same tx or in a competing one,
                    // skip it anyway
                    fAlreadyVoted = true;
                     //LogPrintf("CInstantSend::Vote -- WARNING: We already voted for this outpoint, skipping: txHash=%s, outpoint=%s\n",
                       //     txHash.ToString(), itOutpointLock->first.ToStringShort());
                    break;
                }
            }
        }
        if(fAlreadyVoted) {
            ++itOutpointLock;
            continue; // skip to the next outpoint
        }

        // we haven't voted for this outpoint yet, let's try to do this now
        CTxLockVote vote(txHash, itOutpointLock->first, activeGhostnode.vin.prevout);

        if(!vote.Sign()) {
             //LogPrintf("CInstantSend::Vote -- Failed to sign consensus vote\n");
            return;
        }
        if(!vote.CheckSignature()) {
             //LogPrintf("CInstantSend::Vote -- Signature invalid\n");
            return;
        }

        // vote constructed sucessfully, let's store and relay it
        uint256 nVoteHash = vote.GetHash();
        mapTxLockVotes.insert(std::make_pair(nVoteHash, vote));
        if(itOutpointLock->second.AddVote(vote)) {
             //LogPrintf("CInstantSend::Vote -- Vote created successfully, relaying: txHash=%s, outpoint=%s, vote=%s\n",
                //    txHash.ToString(), itOutpointLock->first.ToStringShort(), nVoteHash.ToString());

            if(itVoted == mapVotedOutpoints.end()) {
                std::set<uint256> setHashes;
                setHashes.insert(txHash);
                mapVotedOutpoints.insert(std::make_pair(itOutpointLock->first, setHashes));
            } else {
                mapVotedOutpoints[itOutpointLock->first].insert(txHash);
                if(mapVotedOutpoints[itOutpointLock->first].size() > 1) {
                    // it's ok to continue, just warn user
                     //LogPrintf("CInstantSend::Vote -- WARNING: Vote conflicts with some existing votes: txHash=%s, outpoint=%s, vote=%s\n",
                          //  txHash.ToString(), itOutpointLock->first.ToStringShort(), nVoteHash.ToString());
                }
            }

            vote.Relay();
        }

        ++itOutpointLock;
    }
}

//received a consensus vote
bool CInstantSend::ProcessTxLockVote(CNode* pfrom, CTxLockVote& vote)
{
    LOCK2(cs_main, cs_instantsend);

    uint256 txHash = vote.GetTxHash();

    if(!vote.IsValid(pfrom)) {
        // could be because of missing MN
         //LogPrintf("instantsend", "CInstantSend::ProcessTxLockVote -- Vote is invalid, txid=%s\n", txHash.ToString());
        return false;
    }

    // Ghostnodes will sometimes propagate votes before the transaction is known to the client,
    // will actually process only after the lock request itself has arrived

    std::map<uint256, CTxLockCandidate>::iterator it = mapTxLockCandidates.find(txHash);
    if(it == mapTxLockCandidates.end()) {
        if(!mapTxLockVotesOrphan.count(vote.GetHash())) {
            mapTxLockVotesOrphan[vote.GetHash()] = vote;
             //LogPrintf("instantsend", "CInstantSend::ProcessTxLockVote -- Orphan vote: txid=%s  ghostnode=%s new\n",
               //     txHash.ToString(), vote.GetGhostnodeOutpoint().ToStringShort());
            bool fReprocess = true;
            std::map<uint256, CTxLockRequest>::iterator itLockRequest = mapLockRequestAccepted.find(txHash);
            if(itLockRequest == mapLockRequestAccepted.end()) {
                itLockRequest = mapLockRequestRejected.find(txHash);
                if(itLockRequest == mapLockRequestRejected.end()) {
                    // still too early, wait for tx lock request
                    fReprocess = false;
                }
            }
            if(fReprocess && IsEnoughOrphanVotesForTx(itLockRequest->second)) {
                // We have enough votes for corresponding lock to complete,
                // tx lock request should already be received at this stage.
                 //LogPrintf("instantsend", "CInstantSend::ProcessTxLockVote -- Found enough orphan votes, reprocessing Transaction Lock Request: txid=%s\n", txHash.ToString());
                ProcessTxLockRequest(itLockRequest->second);
                return true;
            }
        } else {
             //LogPrintf("instantsend", "CInstantSend::ProcessTxLockVote -- Orphan vote: txid=%s  ghostnode=%s seen\n",
               //     txHash.ToString(), vote.GetGhostnodeOutpoint().ToStringShort());
        }

        // This tracks those messages and allows only the same rate as of the rest of the network
        // TODO: make sure this works good enough for multi-quorum

        int nGhostnodeOrphanExpireTime = GetTime() + 60*10; // keep time data for 10 minutes
        if(!mapGhostnodeOrphanVotes.count(vote.GetGhostnodeOutpoint())) {
            mapGhostnodeOrphanVotes[vote.GetGhostnodeOutpoint()] = nGhostnodeOrphanExpireTime;
        } else {
            int64_t nPrevOrphanVote = mapGhostnodeOrphanVotes[vote.GetGhostnodeOutpoint()];
            if(nPrevOrphanVote > GetTime() && nPrevOrphanVote > GetAverageGhostnodeOrphanVoteTime()) {
                 //LogPrintf("instantsend", "CInstantSend::ProcessTxLockVote -- ghostnode is spamming orphan Transaction Lock Votes: txid=%s  ghostnode=%s\n",
                     //   txHash.ToString(), vote.GetGhostnodeOutpoint().ToStringShort());
                // Misbehaving(pfrom->id, 1);
                return false;
            }
            // not spamming, refresh
            mapGhostnodeOrphanVotes[vote.GetGhostnodeOutpoint()] = nGhostnodeOrphanExpireTime;
        }

        return true;
    }

     //LogPrintf("instantsend", "CInstantSend::ProcessTxLockVote -- Transaction Lock Vote, txid=%s\n", txHash.ToString());

    std::map<COutPoint, std::set<uint256> >::iterator it1 = mapVotedOutpoints.find(vote.GetOutpoint());
    if(it1 != mapVotedOutpoints.end()) {
        BOOST_FOREACH(const uint256& hash, it1->second) {
            if(hash != txHash) {
                // same outpoint was already voted to be locked by another tx lock request,
                // find out if the same mn voted on this outpoint before
                std::map<uint256, CTxLockCandidate>::iterator it2 = mapTxLockCandidates.find(hash);
                if(it2->second.HasGhostnodeVoted(vote.GetOutpoint(), vote.GetGhostnodeOutpoint())) {
                    // yes, it did, refuse to accept a vote to include the same outpoint in another tx
                    // from the same ghostnode.
                    // TODO: apply pose ban score to this ghostnode?
                    // NOTE: if we decide to apply pose ban score here, this vote must be relayed further
                    // to let all other nodes know about this node's misbehaviour and let them apply
                    // pose ban score too.
                     //LogPrintf("CInstantSend::ProcessTxLockVote -- ghostnode sent conflicting votes! %s\n", vote.GetGhostnodeOutpoint().ToStringShort());
                    return false;
                }
            }
        }
        // we have votes by other ghostnodes only (so far), let's continue and see who will win
        it1->second.insert(txHash);
    } else {
        std::set<uint256> setHashes;
        setHashes.insert(txHash);
        mapVotedOutpoints.insert(std::make_pair(vote.GetOutpoint(), setHashes));
    }

    CTxLockCandidate& txLockCandidate = it->second;

    if(!txLockCandidate.AddVote(vote)) {
        // this should never happen
        return false;
    }

    int nSignatures = txLockCandidate.CountVotes();
    int nSignaturesMax = txLockCandidate.txLockRequest.GetMaxSignatures();
     //LogPrintf("instantsend", "CInstantSend::ProcessTxLockVote -- Transaction Lock signatures count: %d/%d, vote hash=%s\n",
       //     nSignatures, nSignaturesMax, vote.GetHash().ToString());

    TryToFinalizeLockCandidate(txLockCandidate);

    vote.Relay();

    return true;
}

void CInstantSend::ProcessOrphanTxLockVotes()
{
    LOCK2(cs_main, cs_instantsend);
    std::map<uint256, CTxLockVote>::iterator it = mapTxLockVotesOrphan.begin();
    while(it != mapTxLockVotesOrphan.end()) {
        if(ProcessTxLockVote(NULL, it->second)) {
            mapTxLockVotesOrphan.erase(it++);
        } else {
            ++it;
        }
    }
}

bool CInstantSend::IsEnoughOrphanVotesForTx(const CTxLockRequest& txLockRequest)
{
    // There could be a situation when we already have quite a lot of votes
    // but tx lock request still wasn't received. Let's scan through
    // orphan votes to check if this is the case.
    BOOST_FOREACH(const CTxIn& txin, txLockRequest.vin) {
        if(!IsEnoughOrphanVotesForTxAndOutPoint(txLockRequest.GetHash(), txin.prevout)) {
            return false;
        }
    }
    return true;
}

bool CInstantSend::IsEnoughOrphanVotesForTxAndOutPoint(const uint256& txHash, const COutPoint& outpoint)
{
    // Scan orphan votes to check if this outpoint has enough orphan votes to be locked in some tx.
    LOCK2(cs_main, cs_instantsend);
    int nCountVotes = 0;
    std::map<uint256, CTxLockVote>::iterator it = mapTxLockVotesOrphan.begin();
    while(it != mapTxLockVotesOrphan.end()) {
        if(it->second.GetTxHash() == txHash && it->second.GetOutpoint() == outpoint) {
            nCountVotes++;
            if(nCountVotes >= COutPointLock::SIGNATURES_REQUIRED) {
                return true;
            }
        }
        ++it;
    }
    return false;
}

void CInstantSend::TryToFinalizeLockCandidate(const CTxLockCandidate& txLockCandidate)
{
    LOCK2(cs_main, cs_instantsend);

    uint256 txHash = txLockCandidate.txLockRequest.GetHash();
    if(txLockCandidate.IsAllOutPointsReady() && !IsLockedInstantSendTransaction(txHash)) {
        // we have enough votes now
         //LogPrintf("instantsend", "CInstantSend::TryToFinalizeLockCandidate -- Transaction Lock is ready to complete, txid=%s\n", txHash.ToString());
        if(ResolveConflicts(txLockCandidate, Params().GetConsensus().nInstantSendKeepLock)) {
            LockTransactionInputs(txLockCandidate);
            UpdateLockedTransaction(txLockCandidate);
        }
    }
}

void CInstantSend::UpdateLockedTransaction(const CTxLockCandidate& txLockCandidate)
{
    LOCK(cs_instantsend);

    uint256 txHash = txLockCandidate.GetHash();

    if(!IsLockedInstantSendTransaction(txHash)) return; // not a locked tx, do not update/notify

#ifdef ENABLE_WALLET
    if(GetHDWallet(vpwallets.front())) {
        map<uint256, CWalletTx>::const_iterator mi = GetHDWallet(vpwallets.front())->mapWallet.find(txHash);
        if (mi != GetHDWallet(vpwallets.front())->mapWallet.end())
            GetHDWallet(vpwallets.front())->NotifyTransactionChanged(GetHDWallet(vpwallets.front()), txHash, CT_UPDATED);
        // bumping this to update UI
        nCompleteTXLocks++;
        // notify an external script once threshold is reached
        std::string strCmd = gArgs.GetArg("-instantsendnotify", "");
        if(!strCmd.empty()) {
            boost::replace_all(strCmd, "%s", txHash.GetHex());
            boost::thread t(runCommand, strCmd); // thread runs free
        }
    }
#endif

//    GetMainSignals().NotifyTransactionLock(txLockCandidate.txLockRequest);

     //LogPrintf("instantsend", "CInstantSend::UpdateLockedTransaction -- done, txid=%s\n", txHash.ToString());
}

void CInstantSend::LockTransactionInputs(const CTxLockCandidate& txLockCandidate)
{
    LOCK(cs_instantsend);

    uint256 txHash = txLockCandidate.GetHash();

    if(!txLockCandidate.IsAllOutPointsReady()) return;

    std::map<COutPoint, COutPointLock>::const_iterator it = txLockCandidate.mapOutPointLocks.begin();

    while(it != txLockCandidate.mapOutPointLocks.end()) {
        mapLockedOutpoints.insert(std::make_pair(it->first, txHash));
        ++it;
    }
     //LogPrintf("instantsend", "CInstantSend::LockTransactionInputs -- done, txid=%s\n", txHash.ToString());
}

bool CInstantSend::GetLockedOutPointTxHash(const COutPoint& outpoint, uint256& hashRet)
{
    LOCK(cs_instantsend);
    std::map<COutPoint, uint256>::iterator it = mapLockedOutpoints.find(outpoint);
    if(it == mapLockedOutpoints.end()) return false;
    hashRet = it->second;
    return true;
}

bool CInstantSend::ResolveConflicts(const CTxLockCandidate& txLockCandidate, int nMaxBlocks)
{
    LOCK2(cs_main, cs_instantsend);

    uint256 txHash = txLockCandidate.GetHash();

    // make sure the lock is ready
    if(!txLockCandidate.IsAllOutPointsReady()) return false;

    LOCK(mempool.cs); // protect mempool.mapNextTx

    BOOST_FOREACH(const CTxIn& txin, txLockCandidate.txLockRequest.vin) {
        uint256 hashConflicting;
        if(GetLockedOutPointTxHash(txin.prevout, hashConflicting) && txHash != hashConflicting) {
            // completed lock which conflicts with another completed one?
            // this means that majority of MNs in the quorum for this specific tx input are malicious!
            std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.find(txHash);
            std::map<uint256, CTxLockCandidate>::iterator itLockCandidateConflicting = mapTxLockCandidates.find(hashConflicting);
            if(itLockCandidate == mapTxLockCandidates.end() || itLockCandidateConflicting == mapTxLockCandidates.end()) {
                // safety check, should never really happen
                 //LogPrintf("CInstantSend::ResolveConflicts -- ERROR: Found conflicting completed Transaction Lock, but one of txLockCandidate-s is missing, txid=%s, conflicting txid=%s\n",
                   //     txHash.ToString(), hashConflicting.ToString());
                return false;
            }
             //LogPrintf("CInstantSend::ResolveConflicts -- WARNING: Found conflicting completed Transaction Lock, dropping both, txid=%s, conflicting txid=%s\n",
                    //txHash.ToString(), hashConflicting.ToString());
            CTxLockRequest txLockRequest = itLockCandidate->second.txLockRequest;
            CTxLockRequest txLockRequestConflicting = itLockCandidateConflicting->second.txLockRequest;
            itLockCandidate->second.SetConfirmedHeight(0); // expired
            itLockCandidateConflicting->second.SetConfirmedHeight(0); // expired
            CheckAndRemove(); // clean up
            // AlreadyHave should still return "true" for both of them
            mapLockRequestRejected.insert(make_pair(txHash, txLockRequest));
            mapLockRequestRejected.insert(make_pair(hashConflicting, txLockRequestConflicting));

            // TODO: clean up mapLockRequestRejected later somehow
            //       (not a big issue since we already PoSe ban malicious masternodes
            //        and they won't be able to spam)
            // TODO: ban all malicious masternodes permanently, do not accept anything from them, ever

            // TODO: notify zmq+script about this double-spend attempt
            //       and let merchant cancel/hold the order if it's not too late...

            // can't do anything else, fallback to regular txes
            return false;
        } else if (mempool.mapNextTx.count(txin.prevout)) {
            // check if it's in mempool


            //TODO: find a solution for calling
            //hashConflicting = mempool.mapNextTx[txin.prevout].second.ptx->GetHash();
            if(txHash == hashConflicting) continue; // matches current, not a conflict, skip to next txin
            // conflicts with tx in mempool
             //LogPrintf("CInstantSend::ResolveConflicts -- ERROR: Failed to complete Transaction Lock, conflicts with mempool, txid=%s\n", txHash.ToString());
            return false;
        }
    } // FOREACH
    // No conflicts were found so far, check to see if it was already included in block
    CTransactionRef txTmp;
    uint256 hashBlock;
    if(GetTransaction(txHash, txTmp, Params().GetConsensus(), hashBlock, true) && hashBlock != uint256()) {
         //LogPrintf("instantsend", "CInstantSend::ResolveConflicts -- Done, %s is included in block %s\n", txHash.ToString(), hashBlock.ToString());
        return true;
    }
    // Not in block yet, make sure all its inputs are still unspent
    BOOST_FOREACH(const CTxIn& txin, txLockCandidate.txLockRequest.vin) {
        Coin coin;
        if(!GetUTXOCoin(txin.prevout, coin)) {
            // Not in UTXO anymore? A conflicting tx was mined while we were waiting for votes.
             //LogPrintf("CInstantSend::ResolveConflicts -- ERROR: Failed to find UTXO %s, can't complete Transaction Lock\n", txin.prevout.ToStringShort());
            return false;
        }
    }
     //LogPrintf("instantsend", "CInstantSend::ResolveConflicts -- Done, txid=%s\n", txHash.ToString());

    return true;
}

int64_t CInstantSend::GetAverageGhostnodeOrphanVoteTime()
{
    LOCK(cs_instantsend);
    // NOTE: should never actually call this function when mapGhostnodeOrphanVotes is empty
    if(mapGhostnodeOrphanVotes.empty()) return 0;

    std::map<COutPoint, int64_t>::iterator it = mapGhostnodeOrphanVotes.begin();
    int64_t total = 0;

    while(it != mapGhostnodeOrphanVotes.end()) {
        total+= it->second;
        ++it;
    }

    return total / mapGhostnodeOrphanVotes.size();
}

void CInstantSend::CheckAndRemove()
{
    if(!pCurrentBlockIndex) return;

    LOCK(cs_instantsend);

    std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.begin();

    // remove expired candidates
    while(itLockCandidate != mapTxLockCandidates.end()) {
        CTxLockCandidate &txLockCandidate = itLockCandidate->second;
        uint256 txHash = txLockCandidate.GetHash();
        if(txLockCandidate.IsExpired(pCurrentBlockIndex->nHeight)) {
             //LogPrintf("CInstantSend::CheckAndRemove -- Removing expired Transaction Lock Candidate: txid=%s\n", txHash.ToString());
            std::map<COutPoint, COutPointLock>::iterator itOutpointLock = txLockCandidate.mapOutPointLocks.begin();
            while(itOutpointLock != txLockCandidate.mapOutPointLocks.end()) {
                mapLockedOutpoints.erase(itOutpointLock->first);
                mapVotedOutpoints.erase(itOutpointLock->first);
                ++itOutpointLock;
            }
            mapLockRequestAccepted.erase(txHash);
            mapLockRequestRejected.erase(txHash);
            mapTxLockCandidates.erase(itLockCandidate++);
        } else {
            ++itLockCandidate;
        }
    }

    // remove expired votes
    std::map<uint256, CTxLockVote>::iterator itVote = mapTxLockVotes.begin();
    while(itVote != mapTxLockVotes.end()) {
        if(itVote->second.IsExpired(pCurrentBlockIndex->nHeight)) {
             //LogPrintf("instantsend", "CInstantSend::CheckAndRemove -- Removing expired vote: txid=%s  ghostnode=%s\n",
               //     itVote->second.GetTxHash().ToString(), itVote->second.GetGhostnodeOutpoint().ToStringShort());
            mapTxLockVotes.erase(itVote++);
        } else {
            ++itVote;
        }
    }

    // remove expired orphan votes
    std::map<uint256, CTxLockVote>::iterator itOrphanVote = mapTxLockVotesOrphan.begin();
    while(itOrphanVote != mapTxLockVotesOrphan.end()) {
        if(GetTime() - itOrphanVote->second.GetTimeCreated() > ORPHAN_VOTE_SECONDS) {
             //LogPrintf("instantsend", "CInstantSend::CheckAndRemove -- Removing expired orphan vote: txid=%s  ghostnode=%s\n",
               //     itOrphanVote->second.GetTxHash().ToString(), itOrphanVote->second.GetGhostnodeOutpoint().ToStringShort());
            mapTxLockVotes.erase(itOrphanVote->first);
            mapTxLockVotesOrphan.erase(itOrphanVote++);
        } else {
            ++itOrphanVote;
        }
    }

    // remove expired ghostnode orphan votes (DOS protection)
    std::map<COutPoint, int64_t>::iterator itGhostnodeOrphan = mapGhostnodeOrphanVotes.begin();
    while(itGhostnodeOrphan != mapGhostnodeOrphanVotes.end()) {
        if(itGhostnodeOrphan->second < GetTime()) {
             //LogPrintf("instantsend", "CInstantSend::CheckAndRemove -- Removing expired orphan ghostnode vote: ghostnode=%s\n",
                //    itGhostnodeOrphan->first.ToStringShort());
            mapGhostnodeOrphanVotes.erase(itGhostnodeOrphan++);
        } else {
            ++itGhostnodeOrphan;
        }
    }
}

bool CInstantSend::AlreadyHave(const uint256& hash)
{
    LOCK(cs_instantsend);
    return mapLockRequestAccepted.count(hash) ||
            mapLockRequestRejected.count(hash) ||
            mapTxLockVotes.count(hash);
}

void CInstantSend::AcceptLockRequest(const CTxLockRequest& txLockRequest)
{
    LOCK(cs_instantsend);
    mapLockRequestAccepted.insert(make_pair(txLockRequest.GetHash(), txLockRequest));
}

void CInstantSend::RejectLockRequest(const CTxLockRequest& txLockRequest)
{
    LOCK(cs_instantsend);
    mapLockRequestRejected.insert(make_pair(txLockRequest.GetHash(), txLockRequest));
}

bool CInstantSend::HasTxLockRequest(const uint256& txHash)
{
    CTxLockRequest txLockRequestTmp;
    return GetTxLockRequest(txHash, txLockRequestTmp);
}

bool CInstantSend::GetTxLockRequest(const uint256& txHash, CTxLockRequest& txLockRequestRet)
{
    LOCK(cs_instantsend);

    std::map<uint256, CTxLockCandidate>::iterator it = mapTxLockCandidates.find(txHash);
    if(it == mapTxLockCandidates.end()) return false;

    //TODO: find a solution for calling
    //txLockRequestRet = it->second.txLockRequest;

    return true;
}

bool CInstantSend::GetTxLockVote(const uint256& hash, CTxLockVote& txLockVoteRet)
{
    LOCK(cs_instantsend);

    std::map<uint256, CTxLockVote>::iterator it = mapTxLockVotes.find(hash);
    if(it == mapTxLockVotes.end()) return false;
    txLockVoteRet = it->second;

    return true;
}

bool CInstantSend::IsInstantSendReadyToLock(const uint256& txHash)
{
//    if(!fEnableInstantSend || fLargeWorkForkFound || fLargeWorkInvalidChainFound ||
//        !sporkManager.IsSporkActive(SPORK_2_INSTANTSEND_ENABLED)) return false;

    LOCK(cs_instantsend);
    // There must be a successfully verified lock request
    // and all outputs must be locked (i.e. have enough signatures)
    std::map<uint256, CTxLockCandidate>::iterator it = mapTxLockCandidates.find(txHash);
    return it != mapTxLockCandidates.end() && it->second.IsAllOutPointsReady();
}

bool CInstantSend::IsLockedInstantSendTransaction(const uint256& txHash)
{
//    if(!fEnableInstantSend || fLargeWorkForkFound || fLargeWorkInvalidChainFound ||
//        !sporkManager.IsSporkActive(SPORK_2_INSTANTSEND_ENABLED)) return false;

    LOCK(cs_instantsend);

    // there must be a lock candidate
    std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.find(txHash);
    if(itLockCandidate == mapTxLockCandidates.end()) return false;

    // which should have outpoints
    if(itLockCandidate->second.mapOutPointLocks.empty()) return false;

    // and all of these outputs must be included in mapLockedOutpoints with correct hash
    std::map<COutPoint, COutPointLock>::iterator itOutpointLock = itLockCandidate->second.mapOutPointLocks.begin();
    while(itOutpointLock != itLockCandidate->second.mapOutPointLocks.end()) {
        uint256 hashLocked;
        if(!GetLockedOutPointTxHash(itOutpointLock->first, hashLocked) || hashLocked != txHash) return false;
        ++itOutpointLock;
    }

    return true;
}

int CInstantSend::GetTransactionLockSignatures(const uint256& txHash)
{
    if(!fEnableInstantSend) return -1;
//    if(fLargeWorkForkFound || fLargeWorkInvalidChainFound) return -2;
//    if(!sporkManager.IsSporkActive(SPORK_2_INSTANTSEND_ENABLED)) return -3;

    LOCK(cs_instantsend);

    std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.find(txHash);
    if(itLockCandidate != mapTxLockCandidates.end()) {
        return itLockCandidate->second.CountVotes();
    }

    return -1;
}

bool CInstantSend::IsTxLockRequestTimedOut(const uint256& txHash)
{
    if(!fEnableInstantSend) return false;

    LOCK(cs_instantsend);

    std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.find(txHash);
    if (itLockCandidate != mapTxLockCandidates.end()) {
        return !itLockCandidate->second.IsAllOutPointsReady() &&
                itLockCandidate->second.txLockRequest.IsTimedOut();
    }

    return false;
}

void CInstantSend::Relay(const uint256& txHash)
{
    LOCK(cs_instantsend);

    std::map<uint256, CTxLockCandidate>::const_iterator itLockCandidate = mapTxLockCandidates.find(txHash);
    if (itLockCandidate != mapTxLockCandidates.end()) {
        itLockCandidate->second.Relay();
    }
}

void CInstantSend::UpdatedBlockTip(const CBlockIndex *pindex)
{
    pCurrentBlockIndex = pindex;
}

void CInstantSend::SyncTransaction(const CTransaction& tx, const CBlock* pblock)
{
    // Update lock candidates and votes if corresponding tx confirmed
    // or went from confirmed to 0-confirmed or conflicted.

    if (tx.IsCoinBase()) return;

    LOCK2(cs_main, cs_instantsend);

    uint256 txHash = tx.GetHash();

    // When tx is 0-confirmed or conflicted, pblock is NULL and nHeightNew should be set to -1
    CBlockIndex* pblockindex = pblock ? mapBlockIndex[pblock->GetHash()] : NULL;
    int nHeightNew = pblockindex ? pblockindex->nHeight : -1;

     //LogPrintf("instantsend", "CInstantSend::SyncTransaction -- txid=%s nHeightNew=%d\n", txHash.ToString(), nHeightNew);

    // Check lock candidates
    std::map<uint256, CTxLockCandidate>::iterator itLockCandidate = mapTxLockCandidates.find(txHash);
    if(itLockCandidate != mapTxLockCandidates.end()) {
         //LogPrintf("instantsend", "CInstantSend::SyncTransaction -- txid=%s nHeightNew=%d lock candidate updated\n",
               // txHash.ToString(), nHeightNew);
        itLockCandidate->second.SetConfirmedHeight(nHeightNew);
        // Loop through outpoint locks
        std::map<COutPoint, COutPointLock>::iterator itOutpointLock = itLockCandidate->second.mapOutPointLocks.begin();
        while(itOutpointLock != itLockCandidate->second.mapOutPointLocks.end()) {
            // Check corresponding lock votes
            std::vector<CTxLockVote> vVotes = itOutpointLock->second.GetVotes();
            std::vector<CTxLockVote>::iterator itVote = vVotes.begin();
            std::map<uint256, CTxLockVote>::iterator it;
            while(itVote != vVotes.end()) {
                uint256 nVoteHash = itVote->GetHash();
                 //LogPrintf("instantsend", "CInstantSend::SyncTransaction -- txid=%s nHeightNew=%d vote %s updated\n",
                   //     txHash.ToString(), nHeightNew, nVoteHash.ToString());
                it = mapTxLockVotes.find(nVoteHash);
                if(it != mapTxLockVotes.end()) {
                    it->second.SetConfirmedHeight(nHeightNew);
                }
                ++itVote;
            }
            ++itOutpointLock;
        }
    }

    // check orphan votes
    std::map<uint256, CTxLockVote>::iterator itOrphanVote = mapTxLockVotesOrphan.begin();
    while(itOrphanVote != mapTxLockVotesOrphan.end()) {
        if(itOrphanVote->second.GetTxHash() == txHash) {
             //LogPrintf("instantsend", "CInstantSend::SyncTransaction -- txid=%s nHeightNew=%d vote %s updated\n",
                   // txHash.ToString(), nHeightNew, itOrphanVote->first.ToString());
            mapTxLockVotes[itOrphanVote->first].SetConfirmedHeight(nHeightNew);
        }
        ++itOrphanVote;
    }
}

//
// CTxLockRequest
//

bool CTxLockRequest::IsValid(bool fRequireUnspent) const
{
    if(vout.size() < 1) return false;

    if(vin.size() > WARN_MANY_INPUTS) {
         //LogPrintf("instantsend", "CTxLockRequest::IsValid -- WARNING: Too many inputs: tx=%s", ToString());
    }

    LOCK(cs_main);
    if(!CheckFinalTx(*this)) {
         //LogPrintf("instantsend", "CTxLockRequest::IsValid -- Transaction is not final: tx=%s", ToString());
        return false;
    }

    CAmount nValueIn = 0;
    CAmount nValueOut = 0;

    BOOST_FOREACH(const CTxOut& txout, vout) {
        // InstantSend supports normal scripts and unspendable (i.e. data) scripts.
        // TODO: Look into other script types that are normal and can be included
        if(!txout.scriptPubKey.IsNormalPaymentScript() && !txout.scriptPubKey.IsUnspendable()) {
             //LogPrintf("instantsend", "CTxLockRequest::IsValid -- Invalid Script %s", ToString());
            return false;
        }
        nValueOut += txout.nValue;
    }

    BOOST_FOREACH(const CTxIn& txin, vin) {

        Coin coin;
        int nPrevoutHeight = 0;
        CAmount nValue = 0;

        if(!pcoinsTip->GetCoin(txin.prevout, coin) ||
           coin.out.IsNull()) {
             //LogPrintf("instantsend", "CTxLockRequest::IsValid -- Failed to find UTXO %s\n", txin.prevout.ToStringShort());
            // Normally above sould be enough, but in case we are reprocessing this because of
            // a lot of legit orphan votes we should also check already spent outpoints.
            if(fRequireUnspent) return false;
            CTransactionRef txOutpointCreated;
            uint256 nHashOutpointConfirmed;
            if(!GetTransaction(txin.prevout.hash, txOutpointCreated, Params().GetConsensus(), nHashOutpointConfirmed, true) || nHashOutpointConfirmed == uint256()) {
                 //LogPrintf("instantsend", "CTxLockRequest::IsValid -- Failed to find outpoint %s\n", txin.prevout.ToStringShort());
                return false;
            }
            if(txin.prevout.n >= txOutpointCreated->vout.size()) {
                 //LogPrintf("instantsend", "CTxLockRequest::IsValid -- Outpoint %s is out of bounds, size() = %lld\n",
                        //txin.prevout.ToStringShort(), txOutpointCreated->vout.size());
                return false;
            }
            BlockMap::iterator mi = mapBlockIndex.find(nHashOutpointConfirmed);
            if(mi == mapBlockIndex.end() || !mi->second) {
                // shouldn't happen
                 //LogPrintf("instantsend", "CTxLockRequest::IsValid -- Failed to find block %s for outpoint %s\n",
                       // nHashOutpointConfirmed.ToString(), txin.prevout.ToStringShort());
                return false;
            }
            nPrevoutHeight = mi->second->nHeight;
            nValue = txOutpointCreated->vout[txin.prevout.n].nValue;
        } else {
            nPrevoutHeight = coin.nHeight;
            nValue = coin.out.nValue;
        }

        int nTxAge = chainActive.Height() - nPrevoutHeight + 1;
        // 1 less than the "send IX" gui requires, in case of a block propagating the network at the time
        int nConfirmationsRequired = INSTANTSEND_CONFIRMATIONS_REQUIRED - 1;

        if(nTxAge < nConfirmationsRequired) {
             //LogPrintf("instantsend", "CTxLockRequest::IsValid -- outpoint %s too new: nTxAge=%d, nConfirmationsRequired=%d, txid=%s\n",
                   // txin.prevout.ToStringShort(), nTxAge, nConfirmationsRequired, GetHash().ToString());
            return false;
        }

        nValueIn += nValue;
    }

//    if(nValueOut > sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE)*COIN) {
//         //LogPrintf("instantsend", "CTxLockRequest::IsValid -- Transaction value too high: nValueOut=%d, tx=%s", nValueOut, ToString());
//        return false;
//    }

    if(nValueIn - nValueOut < GetMinFee()) {
         //LogPrintf("instantsend", "CTxLockRequest::IsValid -- did not include enough fees in transaction: fees=%d, tx=%s", nValueOut - nValueIn, ToString());
        return false;
    }

    return true;
}

CAmount CTxLockRequest::GetMinFee() const
{
    CAmount nMinFee = MIN_FEE;
    return std::max(nMinFee, CAmount(vin.size() * nMinFee));
}

int CTxLockRequest::GetMaxSignatures() const
{
    return vin.size() * COutPointLock::SIGNATURES_TOTAL;
}

bool CTxLockRequest::IsTimedOut() const
{
    return GetTime() - nTimeCreated > TIMEOUT_SECONDS;
}

//
// CTxLockVote
//

bool CTxLockVote::IsValid(CNode* pnode) const
{
    if(!mnodeman.Has(CTxIn(outpointGhostnode))) {
         //LogPrintf("instantsend", "CTxLockVote::IsValid -- Unknown ghostnode %s\n", outpointGhostnode.ToStringShort());
        mnodeman.AskForMN(pnode, CTxIn(outpointGhostnode));
        return false;
    }

    int nPrevoutHeight = GetUTXOHeight(outpoint);
    if(nPrevoutHeight == -1) {
         //LogPrintf("instantsend", "CTxLockVote::IsValid -- Failed to find UTXO %s\n", outpoint.ToStringShort());
        // Validating utxo set is not enough, votes can arrive after outpoint was already spent,
        // if lock request was mined. We should process them too to count them later if they are legit.
        CTransactionRef txOutpointCreated;
        uint256 nHashOutpointConfirmed;
        if(!GetTransaction(outpoint.hash, txOutpointCreated, Params().GetConsensus(), nHashOutpointConfirmed, true) || nHashOutpointConfirmed == uint256()) {
             //LogPrintf("instantsend", "CTxLockVote::IsValid -- Failed to find outpoint %s\n", outpoint.ToStringShort());
            return false;
        }
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(nHashOutpointConfirmed);
        if(mi == mapBlockIndex.end() || !mi->second) {
            // not on this chain?
             //LogPrintf("instantsend", "CTxLockVote::IsValid -- Failed to find block %s for outpoint %s\n", nHashOutpointConfirmed.ToString(), outpoint.ToStringShort());
            return false;
        }
        nPrevoutHeight = mi->second->nHeight;
    }

    int nLockInputHeight = nPrevoutHeight + 4;

    int n = mnodeman.GetGhostnodeRank(CTxIn(outpointGhostnode), nLockInputHeight, MIN_INSTANTSEND_PROTO_VERSION);

    if(n == -1) {
        //can be caused by past versions trying to vote with an invalid protocol
         //LogPrintf("instantsend", "CTxLockVote::IsValid -- Outdated ghostnode %s\n", outpointGhostnode.ToStringShort());
        return false;
    }
     //LogPrintf("instantsend", "CTxLockVote::IsValid -- Ghostnode %s, rank=%d\n", outpointGhostnode.ToStringShort(), n);

    int nSignaturesTotal = COutPointLock::SIGNATURES_TOTAL;
    if(n > nSignaturesTotal) {
         //LogPrintf("instantsend", "CTxLockVote::IsValid -- Ghostnode %s is not in the top %d (%d), vote hash=%s\n",
                //outpointGhostnode.ToStringShort(), nSignaturesTotal, n, GetHash().ToString());
        return false;
    }

    if(!CheckSignature()) {
         //LogPrintf("CTxLockVote::IsValid -- Signature invalid\n");
        return false;
    }

    return true;
}

uint256 CTxLockVote::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << txHash;
    ss << outpoint;
    ss << outpointGhostnode;
    return ss.GetHash();
}

bool CTxLockVote::CheckSignature() const
{
    std::string strError;
    std::string strMessage = txHash.ToString() + outpoint.ToStringShort();

    ghostnode_info_t infoMn = mnodeman.GetGhostnodeInfo(CTxIn(outpointGhostnode));

    if(!infoMn.fInfoValid) {
         //LogPrintf("CTxLockVote::CheckSignature -- Unknown Ghostnode: ghostnode=%s\n", outpointGhostnode.ToString());
        return false;
    }

    if(!darkSendSigner.VerifyMessage(infoMn.pubKeyGhostnode, vchGhostnodeSignature, strMessage, strError)) {
         //LogPrintf("CTxLockVote::CheckSignature -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CTxLockVote::Sign()
{
    std::string strError;
    std::string strMessage = txHash.ToString() + outpoint.ToStringShort();

    if(!darkSendSigner.SignMessage(strMessage, vchGhostnodeSignature, activeGhostnode.keyGhostnode)) {
         //LogPrintf("CTxLockVote::Sign -- SignMessage() failed\n");
        return false;
    }

    if(!darkSendSigner.VerifyMessage(activeGhostnode.pubKeyGhostnode, vchGhostnodeSignature, strMessage, strError)) {
         //LogPrintf("CTxLockVote::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

void CTxLockVote::Relay() const
{
//    CInv inv(MSG_TXLOCK_VOTE, GetHash());
//    RelayInv(inv);
}

bool CTxLockVote::IsExpired(int nHeight) const
{
    // Locks and votes expire nInstantSendKeepLock blocks after the block corresponding tx was included into.
    return (nConfirmedHeight != -1) && (nHeight - nConfirmedHeight > Params().GetConsensus().nInstantSendKeepLock);
}

//
// COutPointLock
//

bool COutPointLock::AddVote(const CTxLockVote& vote)
{
    if(mapGhostnodeVotes.count(vote.GetGhostnodeOutpoint()))
        return false;
    mapGhostnodeVotes.insert(std::make_pair(vote.GetGhostnodeOutpoint(), vote));
    return true;
}

std::vector<CTxLockVote> COutPointLock::GetVotes() const
{
    std::vector<CTxLockVote> vRet;
    std::map<COutPoint, CTxLockVote>::const_iterator itVote = mapGhostnodeVotes.begin();
    while(itVote != mapGhostnodeVotes.end()) {
        vRet.push_back(itVote->second);
        ++itVote;
    }
    return vRet;
}

bool COutPointLock::HasGhostnodeVoted(const COutPoint& outpointGhostnodeIn) const
{
    return mapGhostnodeVotes.count(outpointGhostnodeIn);
}

void COutPointLock::Relay() const
{
    std::map<COutPoint, CTxLockVote>::const_iterator itVote = mapGhostnodeVotes.begin();
    while(itVote != mapGhostnodeVotes.end()) {
        itVote->second.Relay();
        ++itVote;
    }
}

//
// CTxLockCandidate
//

void CTxLockCandidate::AddOutPointLock(const COutPoint& outpoint)
{
    mapOutPointLocks.insert(make_pair(outpoint, COutPointLock(outpoint)));
}


bool CTxLockCandidate::AddVote(const CTxLockVote& vote)
{
    std::map<COutPoint, COutPointLock>::iterator it = mapOutPointLocks.find(vote.GetOutpoint());
    if(it == mapOutPointLocks.end()) return false;
    return it->second.AddVote(vote);
}

bool CTxLockCandidate::IsAllOutPointsReady() const
{
    if(mapOutPointLocks.empty()) return false;

    std::map<COutPoint, COutPointLock>::const_iterator it = mapOutPointLocks.begin();
    while(it != mapOutPointLocks.end()) {
        if(!it->second.IsReady()) return false;
        ++it;
    }
    return true;
}

bool CTxLockCandidate::HasGhostnodeVoted(const COutPoint& outpointIn, const COutPoint& outpointGhostnodeIn)
{
    std::map<COutPoint, COutPointLock>::iterator it = mapOutPointLocks.find(outpointIn);
    return it !=mapOutPointLocks.end() && it->second.HasGhostnodeVoted(outpointGhostnodeIn);
}

int CTxLockCandidate::CountVotes() const
{
    // Note: do NOT use vote count to figure out if tx is locked, use IsAllOutPointsReady() instead
    int nCountVotes = 0;
    std::map<COutPoint, COutPointLock>::const_iterator it = mapOutPointLocks.begin();
    while(it != mapOutPointLocks.end()) {
        nCountVotes += it->second.CountVotes();
        ++it;
    }
    return nCountVotes;
}

bool CTxLockCandidate::IsExpired(int nHeight) const
{
    // Locks and votes expire nInstantSendKeepLock blocks after the block corresponding tx was included into.
    return (nConfirmedHeight != -1) && (nHeight - nConfirmedHeight > Params().GetConsensus().nInstantSendKeepLock);
}

void CTxLockCandidate::Relay() const
{
    RelayTransaction(txLockRequest, g_connman.get());
    std::map<COutPoint, COutPointLock>::const_iterator itOutpointLock = mapOutPointLocks.begin();
    while(itOutpointLock != mapOutPointLocks.end()) {
        itOutpointLock->second.Relay();
        ++itOutpointLock;
    }
}
