// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// PoSMiner by Peercoin
// Copyright (c) 2020-2022 The Peercoin developers

#include <node/miner.h>

#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <net_processing.h>
#include <node/interface_ui.h>
#include <node/context.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pos.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <shutdown.h> // ShutdownRequested()
#include <timedata.h>
#include <util/exception.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <util/thread.h>
#include <util/threadnames.h>
#include <wallet/coincontrol.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>
#include <validation.h>
#include <warnings.h>

#include <algorithm>
#include <thread>
#include <utility>

using wallet::CWallet;
using wallet::CWalletTx;
using wallet::COutput;
using wallet::CCoinControl;
using wallet::ReserveDestination;

std::thread m_minter_thread;

namespace node {
static std::atomic<bool> fEnableStaking(false);
bool EnableStaking() { return fEnableStaking; }

int64_t UpdateTime(CBlock* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime{std::max<int64_t>(pindexPrev->GetMedianTimePast() + 1, GetAdjustedTimeSeconds())};

    if (nOldTime < nNewTime) {
        pblock->nTime = nNewTime;
    }

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks) {
        pblock->nBits = GetNextTargetRequired(pindexPrev, consensusParams, pblock->IsProofOfStake());
    }

    return nNewTime - nOldTime;
}

int64_t GetMaxTransactionTime(CBlock* pblock)
{
    int64_t maxTransactionTime = 0;
    for (std::vector<CTransactionRef>::const_iterator it(pblock->vtx.begin()); it != pblock->vtx.end(); ++it)
        maxTransactionTime = std::max(maxTransactionTime, (int64_t)it->get()->nTime);
    return maxTransactionTime;
}

void RegenerateCommitments(CBlock& block, ChainstateManager& chainman)
{
    CMutableTransaction tx{*block.vtx.at(0)};
    tx.vout.erase(tx.vout.begin() + GetWitnessCommitmentIndex(block));
    block.vtx.at(0) = MakeTransactionRef(tx);

    const CBlockIndex* prev_block = WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock));
    chainman.GenerateCoinbaseCommitment(block, prev_block);

    block.hashMerkleRoot = BlockMerkleRoot(block);
}

static BlockAssembler::Options ClampOptions(BlockAssembler::Options options)
{
    // Limit weight to between 4K and DEFAULT_BLOCK_MAX_WEIGHT for sanity:
    options.nBlockMaxWeight = std::clamp<size_t>(options.nBlockMaxWeight, 4000, DEFAULT_BLOCK_MAX_WEIGHT);
    return options;
}

BlockAssembler::BlockAssembler(Chainstate& chainstate, const CTxMemPool* mempool, const Options& options)
    : chainparams{chainstate.m_chainman.GetParams()},
      m_mempool{mempool},
      m_chainstate{chainstate},
      m_options{ClampOptions(options)}
{
}

void ApplyArgsManOptions(const ArgsManager& args, BlockAssembler::Options& options)
{
    // Block resource limits
    options.nBlockMaxWeight = args.GetIntArg("-blockmaxweight", options.nBlockMaxWeight);
    if (const auto blockmintxfee{args.GetArg("-blockmintxfee")}) {
        if (const auto parsed{ParseMoney(*blockmintxfee)}) options.blockMinFeeRate = CFeeRate{*parsed};
    }
}
static BlockAssembler::Options ConfiguredOptions()
{
    BlockAssembler::Options options;
    ApplyArgsManOptions(gArgs, options);
    return options;
}

BlockAssembler::BlockAssembler(Chainstate& chainstate, const CTxMemPool* mempool)
    : BlockAssembler(chainstate, mempool, ConfiguredOptions()) {}

void BlockAssembler::resetBlock()
{
    inBlock.clear();

    // Reserve space for coinbase tx
    nBlockWeight = 4000;
    nBlockSigOpsCost = 400;
    fIncludeWitness = false;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
}

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn, CWallet* pwallet, bool* pfPoSCancel, NodeContext* m_node, int64_t* pFees)
{
    const auto time_start{SteadyClock::now()};

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if (!pblocktemplate.get()) {
        return nullptr;
    }
    CBlock* const pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end

    LOCK(::cs_main);
    CBlockIndex* pindexPrev = m_chainstate.m_chain.Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    pblock->nVersion = m_chainstate.m_chainman.m_versionbitscache.ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand()) {
        pblock->nVersion = gArgs.GetIntArg("-blockversion", pblock->nVersion);
    }

    pblock->nTime = GetAdjustedTimeSeconds();

    const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();

    bool enforce_locktime_median_time_past{false};
    if (chainparams.GetConsensus().IsProtocolV3_1(pblock->nTime)) {
        enforce_locktime_median_time_past = true;
    }

    m_lock_time_cutoff = enforce_locktime_median_time_past ?
                            nMedianTimePast :
                            pblock->GetBlockTime();

    // Decide whether to include witness transactions
    // This is only needed in case the witness softfork activation is reverted
    // (which would require a very deep reorganization).
    // Note that the mempool would accept transactions with witness data before
    // the deployment is active, but we would only ever mine blocks after activation
    // unless there is a massive block reorganization with the witness softfork
    // not activated.
    // TODO: replace this with a call to main to assess validity of a mempool
    // transaction (which in most cases can be a no-op).
    fIncludeWitness = DeploymentActiveAfter(pindexPrev, *m_node->chainman, Consensus::DEPLOYMENT_SEGWIT);

    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    if (m_mempool) {
        LOCK(m_mempool->cs);
        addPackageTxs(*m_mempool, nPackagesSelected, nDescendantsUpdated, pblock->nTime);
    }

    const auto time_1{SteadyClock::now()};

    m_last_block_num_txs = nBlockTx;
    m_last_block_weight = nBlockWeight;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vout.resize(1);

    // Proof-of-work block
    if (!pwallet) {
        pblock->nBits = GetNextTargetRequired(pindexPrev, chainparams.GetConsensus(), false);
        coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;
        coinbaseTx.vout[0].nValue = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
    }

    // Proof-of-stake block
#ifdef ENABLE_WALLET
    // peercoin: if coinstake available add coinstake tx
    static int64_t nLastCoinStakeSearchTime = GetAdjustedTimeSeconds();  // only initialized at startup

    if (pwallet) {
        // flush orphaned coinstakes
        pwallet->AbandonOrphanedCoinstakes();

        // attempt to find a coinstake
        *pfPoSCancel = true;
        pblock->nBits = GetNextTargetRequired(pindexPrev, chainparams.GetConsensus(), true);
        CMutableTransaction txCoinStake;
        txCoinStake.nTime &= ~chainparams.GetConsensus().nStakeTimestampMask;

        int64_t nSearchTime = txCoinStake.nTime; // search to current time

        if (nSearchTime > nLastCoinStakeSearchTime) {
            if (pwallet->CreateCoinStake(*m_node->chainman, pwallet, pblock->nBits, 1, txCoinStake, nFees)) {
                if (txCoinStake.nTime >= pindexPrev->GetMedianTimePast()+1) {
                    // Make the coinbase tx empty in case of proof of stake
                    coinbaseTx.vout[0].SetEmpty();
                    pblock->nTime = coinbaseTx.nTime = txCoinStake.nTime;
                    pblock->vtx.insert(pblock->vtx.begin() + 1, MakeTransactionRef(CTransaction(txCoinStake)));
                    *pfPoSCancel = false;
                }
            }
            pwallet->m_last_coin_stake_search_interval = nSearchTime - nLastCoinStakeSearchTime;
            nLastCoinStakeSearchTime = nSearchTime;
        }
        if (*pfPoSCancel)
            return nullptr; // peercoin: there is no point to continue if we failed to create coinstake
        pblock->nFlags = CBlockIndex::BLOCK_PROOF_OF_STAKE;
    }
#endif

    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));
    if (fIncludeWitness)
        pblocktemplate->vchCoinbaseCommitment = m_chainstate.m_chainman.GenerateCoinbaseCommitment(*pblock, pindexPrev);
    pblocktemplate->vTxFees[0] = -nFees;

    LogPrintf("CreateNewBlock(): block weight: %u txs: %u fees: %ld sigops %d\n", GetBlockWeight(*pblock), nBlockTx, nFees, nBlockSigOpsCost);

    if (pFees)
        *pFees = nFees;

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    pblock->nTime = std::max(pindexPrev->GetMedianTimePast()+1, GetMaxTransactionTime(pblock));
    if (!pblock->IsProofOfStake())
        UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nNonce         = 0;
    pblocktemplate->vTxSigOpsCost[0] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[0]);

    BlockValidationState state;
    if (!pblock->IsProofOfStake() && m_options.test_block_validity && !TestBlockValidity(state, chainparams, m_chainstate, *pblock, pindexPrev,
                                                  GetAdjustedTime, /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, state.ToString()));
    }
    const auto time_2{SteadyClock::now()};

    LogPrint(BCLog::BENCH, "CreateNewBlock() packages: %.2fms (%d packages, %d updated descendants), validity: %.2fms (total %.2fms)\n",
             Ticks<MillisecondsDouble>(time_1 - time_start), nPackagesSelected, nDescendantsUpdated,
             Ticks<MillisecondsDouble>(time_2 - time_1),
             Ticks<MillisecondsDouble>(time_2 - time_start));

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries& testSet)
{
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end(); ) {
        // Only test txs not already in the block
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        } else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, int64_t packageSigOpsCost) const
{
    // TODO: switch to weight-based accounting for packages instead of vsize-based accounting.
    if (nBlockWeight + WITNESS_SCALE_FACTOR * packageSize >= m_options.nBlockMaxWeight) {
        return false;
    }
    if (nBlockSigOpsCost + packageSigOpsCost >= MAX_BLOCK_SIGOPS_COST) {
        return false;
    }
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
// - premature witness (in case segwit transactions are added to mempool before
//   segwit activation)
// - transaction timestamp limit
bool BlockAssembler::TestPackageTransactions(const CTxMemPool::setEntries& package, uint32_t nTime) const
{
    for (CTxMemPool::txiter it : package) {
        if (!IsFinalTx(it->GetTx(), nHeight, m_lock_time_cutoff)) {
            return false;
        }
        if (!fIncludeWitness && it->GetTx().HasWitness()) {
            return false;
        }
        // peercoin: timestamp limit
        if (it->GetTx().nTime > GetAdjustedTimeSeconds() || (nTime && it->GetTx().nTime > nTime)) {
            return false;
        }
    }
    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblocktemplate->block.vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority = gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        LogPrintf("fee rate %s txid %s\n",
                  CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                  iter->GetTx().GetHash().ToString());
    }
}

/** Add descendants of given transactions to mapModifiedTx with ancestor
 * state updated assuming given transactions are inBlock. Returns number
 * of updated descendants. */
static int UpdatePackagesForAdded(const CTxMemPool& mempool,
                                  const CTxMemPool::setEntries& alreadyAdded,
                                  indexed_modified_transaction_set& mapModifiedTx) EXCLUSIVE_LOCKS_REQUIRED(mempool.cs)
{
    AssertLockHeld(mempool.cs);

    int nDescendantsUpdated = 0;
    for (CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc)) {
                continue;
            }
            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                mit = mapModifiedTx.insert(modEntry).first;
            }
            mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
        }
    }
    return nDescendantsUpdated;
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries& package, std::vector<CTxMemPool::txiter>& sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.
void BlockAssembler::addPackageTxs(const CTxMemPool& mempool, int& nPackagesSelected, int& nDescendantsUpdated, uint32_t nTime)
{
    AssertLockHeld(mempool.cs);

    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    CTxMemPool::indexed_transaction_set::index<ancestor_score>::type::iterator mi = mempool.mapTx.get<ancestor_score>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    while (mi != mempool.mapTx.get<ancestor_score>().end() || !mapModifiedTx.empty()) {
        // First try to find a new transaction in mapTx to evaluate.
        //
        // Skip entries in mapTx that are already in a block or are present
        // in mapModifiedTx (which implies that the mapTx ancestor state is
        // stale due to ancestor inclusion in the block)
        // Also skip transactions that we've already failed to add. This can happen if
        // we consider a transaction in mapModifiedTx and it fails: we can then
        // potentially consider it again while walking mapTx.  It's currently
        // guaranteed to fail again, but as a belt-and-suspenders check we put it in
        // failedTx and avoid re-evaluation, since the re-evaluation would be using
        // cached size/sigops/fee values that are not actually correct.
        /** Return true if given transaction from mapTx has already been evaluated,
         * or if the transaction's cached data in mapTx is incorrect. */
        if (mi != mempool.mapTx.get<ancestor_score>().end()) {
            auto it = mempool.mapTx.project<0>(mi);
            assert(it != mempool.mapTx.end());
            if (mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it)) {
                ++mi;
                continue;
            }
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score>().begin();
        if (mi == mempool.mapTx.get<ancestor_score>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score>().end() &&
                    CompareTxMemPoolEntryByAncestorFee()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        int64_t packageSigOpsCost = iter->GetSigOpCostWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOpsCost = modit->nSigOpCostWithAncestors;
        }

        if (packageFees < m_options.blockMinFeeRate.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            return;
        }

        if (!TestPackage(packageSize, packageSigOpsCost)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx,
                // we must erase failed entries so that we can consider the
                // next best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }

            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight >
                    m_options.nBlockMaxWeight - 4000) {
                // Give up if we're close to full and haven't succeeded in a while
                break;
            }
            continue;
        }

        auto ancestors{mempool.AssumeCalculateMemPoolAncestors(__func__, *iter, CTxMemPool::Limits::NoLimits(), /*fSearchForParents=*/false)};

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final
        if (!TestPackageTransactions(ancestors, nTime)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;

        // Package can be added. Sort the entries in a valid order.
        std::vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, sortedEntries);

        for (size_t i = 0; i < sortedEntries.size(); ++i) {
            AddToBlock(sortedEntries[i]);
            // Erase from the modified set, if present
            mapModifiedTx.erase(sortedEntries[i]);
        }

        ++nPackagesSelected;

        // Update transactions that depend on each of these
        nDescendantsUpdated += UpdatePackagesForAdded(mempool, ancestors, mapModifiedTx);
    }
}

void IncrementExtraNonce(CBlock* pblock, const CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock) {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight + 1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce));
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}

// Peercoin/Blackcoin
static bool ProcessBlockFound(const CBlock* pblock, NodeContext& m_node)
{
    LogPrintf("%s", pblock->ToString());

    // Found a solution
    {
        LOCK(cs_main);
        BlockValidationState state;
        if (!CheckProofOfStake(&m_node.chainman->BlockIndex()[pblock->hashPrevBlock], *pblock->vtx[1], pblock->nBits, state, m_node.chainman->ActiveChainstate().CoinsTip(), pblock->vtx[1]->nTime ? pblock->vtx[1]->nTime : pblock->nTime))
            return error("ProcessBlockFound(): proof-of-stake checking failed");
        
        if (pblock->hashPrevBlock != m_node.chainman->ActiveChain().Tip()->GetBlockHash())
            return error("ProcessBlockFound(): generated block is stale");
    }

    // Process this block the same as if we had received it from another node
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(*pblock);
    if (!m_node.chainman->ProcessNewBlock(shared_pblock, true, true, nullptr))
        return error("ProcessBlockFound(): block not accepted");

    return true;
}

#ifdef ENABLE_WALLET
void PoSMiner(std::shared_ptr<CWallet> pwallet, NodeContext& m_node)
{
    CConnman* connman = m_node.connman.get();
    LogPrintf("PoSMiner started for proof-of-stake\n");
    util::ThreadRename("blackcoin-stake-miner");

    unsigned int nExtraNonce = 0;

    OutputType output_type = pwallet->m_default_change_type ? *pwallet->m_default_change_type : pwallet->m_default_address_type;
    ReserveDestination reservedest(pwallet.get(), output_type);
    CTxDestination dest;

    // Compute timeout for pos as sqrt(numUTXO)
    unsigned int pos_timio;
    {
        LOCK2(pwallet->cs_wallet, cs_main);
        bilingual_str dest_err;
        if (!reservedest.GetReservedDestination(true))
            throw std::runtime_error("Error: Keypool ran out, please call keypoolrefill first");

        std::vector<std::pair<const CWalletTx*, unsigned int> > vCoins;
        CCoinControl coincontrol;
        AvailableCoinsForStaking(*pwallet, vCoins, &coincontrol);
        pos_timio = gArgs.GetIntArg("-staketimio", DEFAULT_STAKETIMIO) + 30 * sqrt(vCoins.size());
        LogPrintf("Set proof-of-stake timeout: %ums for %u UTXOs\n", pos_timio, vCoins.size());
    }

    std::string strMintMessage = _("Info: Staking suspended due to locked wallet").translated;
    std::string strMintSyncMessage = _("Info: Staking suspended while synchronizing wallet").translated;
    std::string strMintDisabledMessage = _("Info: Staking disabled by 'nostaking' option").translated;
    std::string strMintBlockMessage = _("Info: Staking suspended due to block creation failure").translated;
    std::string strMintEmpty = "";
    if (!gArgs.GetBoolArg("-staking", DEFAULT_STAKE))
    {
        strMintWarning = strMintDisabledMessage;
        LogPrintf("proof-of-stake miner disabled\n");
        return;
    }

    try {
        bool fNeedToClear = false;
        while (EnableStaking()) {
            if (ShutdownRequested())
                return;
            while (pwallet->IsLocked()) {
                if (ShutdownRequested() || !EnableStaking())
                    return;
                if (strMintWarning != strMintMessage) {
                    strMintWarning = strMintMessage;
                    uiInterface.NotifyAlertChanged();
                }
                fNeedToClear = true;
                if (!connman->interruptNet.sleep_for(std::chrono::seconds(5)))
                    return;
            }

            // Busy-wait for the network to come online so we don't waste time mining
            // on an obsolete chain. In regtest mode we expect to fly solo.
            while(connman == nullptr || connman->GetNodeCount(ConnectionDirection::Both) == 0 || m_node.chainman->ActiveChainstate().IsInitialBlockDownload()) {
                if (ShutdownRequested()|| !EnableStaking())
                    return;
                while (connman == nullptr) {UninterruptibleSleep(1s);}
                if (strMintWarning != strMintSyncMessage) {
                    strMintWarning = strMintSyncMessage;
                    uiInterface.NotifyAlertChanged();
                }
                fNeedToClear = true;
                if (!connman->interruptNet.sleep_for(std::chrono::seconds(10)))
                    return;
            }

            while (GuessVerificationProgress(Params().TxData(), m_node.chainman->ActiveChain().Tip()) < 0.996)
            {
                if (ShutdownRequested() || !EnableStaking())
                    return;
                LogPrintf("Staker thread sleeps while sync at %f\n", GuessVerificationProgress(Params().TxData(), m_node.chainman->ActiveChain().Tip()));
                if (strMintWarning != strMintSyncMessage) {
                    strMintWarning = strMintSyncMessage;
                    uiInterface.NotifyAlertChanged();
                }
                fNeedToClear = true;
                if (!connman->interruptNet.sleep_for(std::chrono::seconds(10)))
                    return;
            }
            if (fNeedToClear) {
                strMintWarning = strMintEmpty;
                uiInterface.NotifyAlertChanged();
                fNeedToClear = false;
            }

            //
            // Create new block
            //
            CBlockIndex* pindexPrev = m_node.chainman->ActiveChain().Tip();
            bool fPoSCancel = false;
            CScript scriptPubKey = GetScriptForDestination(dest);
            CBlock *pblock;
            std::unique_ptr<CBlockTemplate> pblocktemplate;

            {
                LOCK2(pwallet->cs_wallet, cs_main);
                try {
                    pblocktemplate = BlockAssembler{m_node.chainman->ActiveChainstate(), m_node.mempool.get()}.CreateNewBlock(scriptPubKey, pwallet.get(), &fPoSCancel, &m_node);
                }
                catch (const std::runtime_error &e)
                {
                    LogPrintf("PoSMiner runtime error: %s\n", e.what());
                    continue;
                }
            }

            if (!pblocktemplate.get())
            {
                if (fPoSCancel == true)
                {
                    if (!connman->interruptNet.sleep_for(std::chrono::milliseconds(pos_timio)))
                        return;
                    continue;
                }
                strMintWarning = strMintBlockMessage;
                uiInterface.NotifyAlertChanged();
                LogPrintf("Error in PoSMiner: Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                if (!connman->interruptNet.sleep_for(std::chrono::seconds(10)))
                   return;

                return;
            }
            pblock = &pblocktemplate->block;
            IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

            // peercoin: if proof-of-stake block found then process block
            if (pblock->IsProofOfStake())
            {
                {
                    LOCK2(pwallet->cs_wallet, cs_main);
                    if (!SignBlock(*pblock, *pwallet))
                    {
                        LogPrintf("PoSMiner: failed to sign PoS block");
                        continue;
                    }
                }
                LogPrintf("PoSMiner: proof-of-stake block found %s\n", pblock->GetHash().ToString());
                ProcessBlockFound(pblock, m_node);
                // Rest for ~16 seconds after successful block to preserve close quick
                if (!connman->interruptNet.sleep_for(std::chrono::seconds(16 + GetRand(4))))
                    return;
            }
            if (!connman->interruptNet.sleep_for(std::chrono::milliseconds(pos_timio)))
                return;

            continue;
        }
    }
    catch (const std::runtime_error &e)
    {
        LogPrintf("PoSMiner: runtime error: %s\n", e.what());
        return;
    }
}

// peercoin: stake minter thread
void static ThreadStakeMiner(std::shared_ptr<CWallet> pwallet, NodeContext& m_node)
{
    LogPrintf("ThreadStakeMiner started\n");
    while (true) {
        try {
            PoSMiner(pwallet, m_node);
            break;
        }
        catch (std::exception& e) {
            PrintExceptionContinue(&e, "ThreadStakeMiner()");
        } catch (...) {
            PrintExceptionContinue(nullptr, "ThreadStakeMiner()");
        }
    }
    LogPrintf("ThreadStakeMiner stopped\n");
}

// peercoin: stake minter
void MinePoS(bool fGenerate, std::shared_ptr<CWallet> pwallet, NodeContext& m_node)
{
    if (!WITH_LOCK(pwallet->cs_wallet, return pwallet->GetKeyPoolSize())) {
        LogPrintf("Error: Keypool is empty, please make sure the wallet contains keys and call keypoolrefill before restarting the mining thread\n");
        fEnableStaking = false;
        return;
    }

    if (!fGenerate || pwallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        fEnableStaking = false;
        return;
    }

    if (!EnableStaking()) {
        fEnableStaking = true;
        // peercoin: mint proof-of-stake blocks in the background
        m_minter_thread = std::thread([&] { util::TraceThread("minter", [&] { ThreadStakeMiner(pwallet, m_node); }); });
    }
}

void InterruptStaking()
{
    LogPrintf("Interrupting ThreadStakeMiner\n");
    fEnableStaking = false;
    if (m_minter_thread.joinable()) {
        LogPrintf("Waiting for *interrupt* ThreadStakeMiner...\n");
        m_minter_thread.join();
    }
    LogPrintf("ThreadStakeMiner *interrupt* done!\n");
}

void StopStaking()
{
    LogPrintf("Stopping ThreadStakeMiner\n");
    if (m_minter_thread.joinable()) {
        LogPrintf("Waiting for *stop* ThreadStakeMiner...\n");
        m_minter_thread.join();
    }
    LogPrintf("ThreadStakeMiner *stop* done!\n");
}
#endif

} // namespace node
