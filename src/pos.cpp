// Copyright (c) 2014-2018 The BlackCoin Developers
// Copyright (c) 2011-2013 The PPCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Stake cache by Qtum
// Copyright (c) 2016-2018 The Qtum developers

#include <pos.h>
#include <txdb.h>
#include <chain.h>
#include <chainparams.h>
#include <clientversion.h>
#include <coins.h>
#include <hash.h>
#include <timedata.h>
#include <validation.h>
#include <arith_uint256.h>
#include <uint256.h>
#include <primitives/transaction.h>
#include <script/sign.h>
#include <consensus/consensus.h>
#include <stdio.h>

using namespace std;

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifier(const CBlockIndex* pindexPrev, const uint256& kernel)
{
    if (!pindexPrev)
        return uint256(); // genesis block's modifier is 0

    CHashWriter ss{};
    ss << kernel << pindexPrev->nStakeModifier;
    return ss.GetHash();
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx)
{
    const Consensus::Params& params = Params().GetConsensus();
    if (params.IsProtocolV2(nTimeBlock))
        return (nTimeBlock == nTimeTx) && ((nTimeTx & params.nStakeTimestampMask) == 0);
    else
        return (nTimeBlock == nTimeTx);
}

// Simplified version of CheckCoinStakeTimestamp() to check header-only timestamp
bool CheckStakeBlockTimestamp(int64_t nTimeBlock)
{
   return CheckCoinStakeTimestamp(nTimeBlock, nTimeBlock);
}

// BlackCoin kernel protocol v3
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coins one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                   future proof-of-stake
//   txPrev.nTime: slightly scrambles computation
//   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
//                     generating coinstake at the same time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   nTime: current timestamp
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash(const CBlockIndex* pindexPrev, unsigned int nBits, uint32_t blockFromTime, CAmount prevoutValue, const COutPoint& prevout, unsigned int nTimeTx, bool fPrintProofOfStake)
{
    if (nTimeTx < blockFromTime)  // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");

    // Base target
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);

    // Weighted target
    int64_t nValueIn = prevoutValue;
    if (nValueIn == 0)
        return error("CheckStakeKernelHash() : nValueIn = 0");
    arith_uint256 bnWeight = arith_uint256(nValueIn);
    bnTarget *= bnWeight;

    uint256 nStakeModifier = pindexPrev->nStakeModifier;

    // Calculate hash
    CHashWriter ss{};
    ss << nStakeModifier;
    ss << blockFromTime << prevout.hash << prevout.n << nTimeTx;

    uint256 hashProofOfStake = ss.GetHash();

    if (fPrintProofOfStake)
    {
        LogPrintf("CheckStakeKernelHash() : nStakeModifier=%s, txPrev.nTime=%u, txPrev.vout.hash=%s, txPrev.vout.n=%u, nTimeTx=%u, hashProof=%s\n",
            nStakeModifier.GetHex().c_str(),
            blockFromTime, prevout.hash.ToString(), prevout.n, nTimeTx,
            hashProofOfStake.ToString());
    }

    // Now check if proof-of-stake hash meets target protocol
    if (UintToArith256(hashProofOfStake) > bnTarget)
        return false;
        
    if (LogInstance().WillLogCategory(BCLog::COINSTAKE) && !fPrintProofOfStake)
    {
        LogPrintf("CheckStakeKernelHash() : nStakeModifier=%s, txPrev.nTime=%u, txPrev.vout.hash=%s, txPrev.vout.n=%u, nTimeTx=%u, hashProof=%s\n",
            nStakeModifier.GetHex().c_str(),
            blockFromTime, prevout.hash.ToString(), prevout.n, nTimeTx,
            hashProofOfStake.ToString());
    }

    return true;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, const CTransaction& tx, unsigned int nBits, BlockValidationState& state, CCoinsViewCache& view, unsigned int nTimeTx)
{
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx.vin[0];
    
    Coin coinPrev;

    if (!view.GetCoin(txin.prevout, coinPrev)) {
        return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, "stake-prevout-not-exist", strprintf("CheckProofOfStake() : Stake prevout does not exist %s", txin.prevout.hash.ToString()));
    }

    // Min age requirement
    if (pindexPrev->nHeight + 1 - coinPrev.nHeight < Params().GetConsensus().nCoinbaseMaturity){
        return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, "stake-prevout-not-mature", strprintf("CheckProofOfStake() : Stake prevout is not mature, expecting %i and only matured to %i", Params().GetConsensus().nCoinbaseMaturity, pindexPrev->nHeight + 1 - coinPrev.nHeight));
    }

    CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
    if (!blockFrom) {
        return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, "stake-prevout-not-loaded", strprintf("CheckProofOfStake() : Block at height %i for prevout can not be loaded", coinPrev.nHeight));
    }

    // Verify signature
    if (!VerifySignature(coinPrev, txin.prevout.hash, tx, 0, SCRIPT_VERIFY_NONE))
        return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, "stake-verify-signature-failed", strprintf("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString()));

    if (!CheckStakeKernelHash(pindexPrev, nBits, (coinPrev.nTime ? coinPrev.nTime : blockFrom->nTime), coinPrev.out.nValue, txin.prevout, nTimeTx, LogInstance().WillLogCategory(BCLog::COINSTAKE)))
        return state.Invalid(BlockValidationResult::BLOCK_HEADER_SYNC, "stake-check-kernel-failed", strprintf("CheckProofOfStake() : INFO: check kernel failed on coinstake %s", tx.GetHash().ToString())); // may occur during initial download or if behind on block chain sync

    return true;
}

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTime, const COutPoint& prevout, CCoinsViewCache& view){
    std::map<COutPoint, CStakeCache> tmp;
    return CheckKernel(pindexPrev, nBits, nTime, prevout, view, tmp);
}

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTime, const COutPoint& prevout, CCoinsViewCache& view, const std::map<COutPoint, CStakeCache>& cache)
{
    auto it = cache.find(prevout);

    if (it == cache.end()) {
        // not found in cache (shouldn't happen during staking, only during verification which does not use cache)
        Coin coinPrev;
        if (!view.GetCoin(prevout, coinPrev)) {
            return false;
        }

        if (pindexPrev->nHeight + 1 - coinPrev.nHeight < Params().GetConsensus().nCoinbaseMaturity) {
            return error("CheckKernel(): Coin is not mature");
        }

        CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
        if (!blockFrom) {
            return error("CheckKernel(): Could not find block");
        }

        if (coinPrev.IsSpent()) {
            return error("CheckKernel(): Coin is spent");
        }

        return CheckStakeKernelHash(pindexPrev, nBits, (coinPrev.nTime ? coinPrev.nTime : blockFrom->nTime), coinPrev.out.nValue, prevout, nTime);
    } else {
        // found in cache
        const CStakeCache& stake = it->second;
        if (CheckStakeKernelHash(pindexPrev, nBits, stake.blockFromTime, stake.amount, prevout, nTime)) {
            // Cache could potentially cause false positive stakes in the event of deep reorgs, so check without cache also return CheckKernel(pindexPrev, nBits, nTime, prevout, view); }
            return CheckKernel(pindexPrev, nBits, nTime, prevout, view);
        }
    }
    return false;
}

void CacheKernel(std::map<COutPoint, CStakeCache>& cache, const COutPoint& prevout, CBlockIndex* pindexPrev, CCoinsViewCache& view)
{
    if (cache.find(prevout) != cache.end()) {
        //already in cache
        return;
    }

    Coin coinPrev;
    if (!view.GetCoin(prevout, coinPrev)) {
        return;
    }

    if (pindexPrev->nHeight + 1 - coinPrev.nHeight < Params().GetConsensus().nCoinbaseMaturity) {
        return;
    }

    CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
    if (!blockFrom) {
        return;
    }

    CStakeCache c((coinPrev.nTime ? coinPrev.nTime : blockFrom->nTime), coinPrev.out.nValue);
    cache.insert({prevout, c});
}
