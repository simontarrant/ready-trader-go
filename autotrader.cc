// Copyright 2021 Optiver Asia Pacific Pty. Ltd.
//
// This file is part of Ready Trader Go.
//
//     Ready Trader Go is free software: you can redistribute it and/or
//     modify it under the terms of the GNU Affero General Public License
//     as published by the Free Software Foundation, either version 3 of
//     the License, or (at your option) any later version.
//
//     Ready Trader Go is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU Affero General Public License for more details.
//
//     You should have received a copy of the GNU Affero General Public
//     License along with Ready Trader Go.  If not, see
//     <https://www.gnu.org/licenses/>.
#include <array>

#include <boost/asio/io_context.hpp>

#include <ready_trader_go/logging.h>

#include "autotrader.h"

#include <ctime>
#include <algorithm>
#include <iostream>

using namespace ReadyTraderGo;

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

constexpr int LOT_SIZE = 10;
constexpr unsigned long POSITION_LIMIT = 100;
constexpr int TICK_SIZE_IN_CENTS = 100;
constexpr int MIN_BID_NEARST_TICK = (MINIMUM_BID + TICK_SIZE_IN_CENTS) / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int MAX_ASK_NEAREST_TICK = MAXIMUM_ASK / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;

AutoTrader::AutoTrader(boost::asio::io_context& context) : BaseAutoTrader(context)
{
}

void AutoTrader::DisconnectHandler()
{
    BaseAutoTrader::DisconnectHandler();
    RLOG(LG_AT, LogLevel::LL_INFO) << "execution connection lost";
}

void AutoTrader::ErrorMessageHandler(unsigned long clientOrderId,
                                     const std::string& errorMessage)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "error with order " << clientOrderId << ": " << errorMessage;
    if (clientOrderId != 0 && ((mAsks.count(clientOrderId) == 1) || (mBids.count(clientOrderId) == 1)))
    {
        OrderStatusMessageHandler(clientOrderId, 0, 0, 0);
    }
}

void AutoTrader::HedgeFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "hedge order " << clientOrderId << " filled for " << volume
                                   << " lots at $" << price << " average price in cents";
}

// Handles main logic when order book info comes through about futures or ETF
// Note: Futures appear to come through first on each tick
// What if we want to icnrease the volume on one of our makes because there is more availbale to us now???
// Maybe on the futures call, just make adjustmetns based on the futures, then etf call make adjustments needed
void AutoTrader::OrderBookMessageHandler(Instrument instrument,
                                         unsigned long sequenceNumber,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
    // Copy futures info into attributes to use when the etf order message comes through after
    // See if any current order need to be altered
    if (instrument == Instrument::FUTURE) {
        
        // No future asks and we have a etf ask -> cancel our ask
        if (!askPrices[0] && mAskPrice) {
            cancelOrder(mAskId, true);
        }
        // We have an ask and there is future ask and our ask is below the futures ask (bad) -> cancel our ask
        if (mAskPrice && askPrices[0] && mAskPrice <= askPrices[0]) {
            cancelOrder(mAskId, true);
        }

        // No future bids and we have a etf bid -> cancel our bid
        if (!bidPrices[0] && mBidPrice) {
            cancelOrder(mBidId, false);
        }
        // We have a bid and there is future id and our bid is above the futures bid (bad) -> cancel our bid
        if (mBidPrice && bidPrices[0] && mBidPrice >= bidPrices[0]) {
            cancelOrder(mBidId, false);
        }

        // Copy in futures values to be used when etf info comes through
        // std::copy(askPrices.begin(), askPrices.end(), prevAskPrices.begin());
        // std::copy(askVolumes.begin(), askVolumes.end(), prevAskVolumes.begin());
        // std::copy(bidPrices.begin(), bidPrices.end(), prevBidPrices.begin());
        // std::copy(bidVolumes.begin(), bidVolumes.end(), prevBidVolumes.begin());
        futAskPrice = askPrices[0];
        futAskVol = askVolumes[0];
        futBidPrice = bidPrices[0];
        futBidVol = bidVolumes[0];
    }
    // See if we can make a market with new orders
    else {

        // RLOG(LG_AT, LogLevel::LL_INFO) << "mAskPrice " << mAskPrice << "  futAskPrice" << futAskPrice;


        // 1. Do we need to modify an existing order to increase the spread
        // 2. Has our order gone out of the money and a new one need to be placed

        // no current ask -> create a new one
        if (mAskId == 0) {
            if (futAskPrice != 0) {
                RLOG(LG_AT, LogLevel::LL_INFO) << "no current ask, create a new one";
                RLOG(LG_AT, LogLevel::LL_INFO) << "ASK 1";
                makeAsk(askPrices[0]);
            }
        }
        else {

            // Create a better spread (increase ask)
            if (mAskPrice < askPrices[0] - TICK_SIZE_IN_CENTS) {
                RLOG(LG_AT, LogLevel::LL_INFO) << "ASK 2";
                setUpAwaitingCancelOrder(mAskId, askPrices[0] - TICK_SIZE_IN_CENTS, true);
                cancelOrder(mAskId, true);
                // mAskId = ++mNextMessageId;
                // unsigned long makeAskVol = maxAskVol();
                // RLOG(LG_AT, LogLevel::LL_INFO) << "ASK 2";
                // mAsks.insert(mAskId);
                // SendInsertOrder(mAskId, Side::SELL, askPrices[0] - TICK_SIZE_IN_CENTS, makeAskVol, Lifespan::GOOD_FOR_DAY);
            }

            // Competitors have asks on same level or lower
            // If there is room to move our ask down but still be above futures price
            // Then move it down
            if (mAskPrice >= askPrices[0] && mAskPrice > futAskPrice + TICK_SIZE_IN_CENTS) {
                RLOG(LG_AT, LogLevel::LL_INFO) << "ASK 3";
                setUpAwaitingCancelOrder(mAskId, getMakeAskPrice(askPrices[0]), true);
                cancelOrder(mAskId, true);
                RLOG(LG_AT, LogLevel::LL_INFO) << "ASK 3";
                // makeAsk(askPrices[0]);
            }
        }

        // No current bid
        std::cout << bidPrices[0] << std::endl;
        if (!mBidId) {
            if (futBidPrice) {
                RLOG(LG_AT, LogLevel::LL_INFO) << "BID 1: " << bidPrices[0];
                makeBid(bidPrices[0]);
            }
        }
        else {
            // Create a better spread, lower bid
            if (mBidPrice > bidPrices[0] + TICK_SIZE_IN_CENTS) {
                setUpAwaitingCancelOrder(mBidId, bidPrices[0] + TICK_SIZE_IN_CENTS, false);
                cancelOrder(mBidId, false);
                // mBidId = ++mNextMessageId;
                // unsigned long makeBidVol = maxBidVol();
                // RLOG(LG_AT, LogLevel::LL_INFO) << "BID 2: " << bidPrices[0];
                // SendInsertOrder(mBidId, Side::BUY, bidPrices[0] + TICK_SIZE_IN_CENTS, makeBidVol, Lifespan::GOOD_FOR_DAY);
                // mBids.insert(mBidId);
            }

            // Better bids have been made
            // if there is room to move our bid up but not above fut price - 1
            // Then move bid up to a competitive price or one below fut bid price
            if (mBidPrice <= bidPrices[0] && mBidPrice < futBidPrice - TICK_SIZE_IN_CENTS) {
                setUpAwaitingCancelOrder(mBidId, getMakeBidPrice(bidPrices[0]), false);
                cancelOrder(mBidId, false);
                RLOG(LG_AT, LogLevel::LL_INFO) << "BID 3: " << bidPrices[0];
                // makeBid(bidPrices[0]);
            }
        } 
    }
}

void AutoTrader::makeAsk(unsigned long etfBestAskPrice) {

    unsigned long makeAskVol = maxAskVol();
    if (!makeAskVol) return;

    // How high do we put aks if there are no asks?????
    // if (!etfBestAskPrice) mAskPrice = futAskPrice + TICK_SIZE_IN_CENTS;
    // else mAskPrice = std::max(futAskPrice + TICK_SIZE_IN_CENTS, etfBestAskPrice - TICK_SIZE_IN_CENTS);
    mAskPrice = getMakeAskPrice(etfBestAskPrice);

    mAskId = ++mNextMessageId;

    RLOG(LG_AT, LogLevel::LL_INFO) << "Sending ask: " << mAskId << " Price:" << mAskPrice << " Volume:" << makeAskVol;
    SendInsertOrder(mAskId, Side::SELL, mAskPrice, makeAskVol, Lifespan::GOOD_FOR_DAY);
    mAsks.insert(mAskId);

    mMakeAskAwaitingCancelId = 0;

}

unsigned long AutoTrader::getMakeAskPrice(unsigned long etfBestAskPrice) {
    if (!etfBestAskPrice) return futAskPrice + TICK_SIZE_IN_CENTS;
    else return std::max(futAskPrice + TICK_SIZE_IN_CENTS, etfBestAskPrice - TICK_SIZE_IN_CENTS);

}

unsigned long AutoTrader::maxAskVol() {
    RLOG(LG_AT, LogLevel::LL_INFO) << "Max ask volume: " << std::min(POSITION_LIMIT + etfPosition, futAskVol);
    return std::min(POSITION_LIMIT + etfPosition, futAskVol);
}

void AutoTrader::makeBid(unsigned long etfBestBidPrice) {

    unsigned long makeBidVol = maxBidVol();
    if (!makeBidVol) return;

    // if (!etfBestBidPrice) mBidPrice = futBidPrice - 10 * TICK_SIZE_IN_CENTS;
    // else mBidPrice = std::min(futBidPrice - TICK_SIZE_IN_CENTS, etfBestBidPrice + TICK_SIZE_IN_CENTS);
    mBidPrice = getMakeBidPrice(etfBestBidPrice);

    mBidId = ++mNextMessageId;

    RLOG(LG_AT, LogLevel::LL_INFO) << "Sending bid: " << mBidId << " Price:" << mBidPrice << " Volume:" << makeBidVol;
    SendInsertOrder(mBidId, Side::BUY, mBidPrice, makeBidVol, Lifespan::GOOD_FOR_DAY);
    mBids.insert(mBidId);

    mMakeBidAwaitingCancelId = 0;

}

unsigned long AutoTrader::getMakeBidPrice(unsigned long etfBestBidPrice) {
    if (!etfBestBidPrice) return futBidPrice - 10 * TICK_SIZE_IN_CENTS;
    return std::min(futBidPrice - TICK_SIZE_IN_CENTS, etfBestBidPrice + TICK_SIZE_IN_CENTS);
}

unsigned long AutoTrader::maxBidVol() {
    return std::min(POSITION_LIMIT - etfPosition, futBidVol);
}

void AutoTrader::replaceCancelledTrade(bool ask, unsigned long remainingVol) {

    if (ask) {

        RLOG(LG_AT, LogLevel::LL_INFO) << "Ask: " << mMakeAskAwaitingCancelId << "Cancelled, rem vol: " << remainingVol;

        unsigned long makeAskVol = maxAskVol();
        if (makeAskVol <= remainingVol) return;
        makeAskVol -= remainingVol;

        mAskId = ++mNextMessageId;
        RLOG(LG_AT, LogLevel::LL_INFO) << "Sending ask: " << mAskId << " Price:" << mMakeAskAwaitingCancelPrice << " Volume:" << makeAskVol;
        SendInsertOrder(mAskId, Side::SELL, mMakeAskAwaitingCancelPrice, makeAskVol, Lifespan::GOOD_FOR_DAY);
        mMakeAskAwaitingCancelId = 0;
        mAsks.insert(mAskId);

    } else {
        RLOG(LG_AT, LogLevel::LL_INFO) << "Bid: " << mMakeBidAwaitingCancelId << "Cancelled, rem vol: " << remainingVol;

        unsigned long makeBidVol = maxBidVol();
        if (makeBidVol <= remainingVol) return;
        makeBidVol -= remainingVol;

        mBidId = ++mNextMessageId;
        RLOG(LG_AT, LogLevel::LL_INFO) << "Sending bid: " << mBidId << " Price:" << mMakeBidAwaitingCancelPrice << " Volume:" << makeBidVol;
        SendInsertOrder(mBidId, Side::BUY, mMakeBidAwaitingCancelPrice, makeBidVol, Lifespan::GOOD_FOR_DAY);
        mMakeBidAwaitingCancelId = 0;
        mBids.insert(mBidId);

    }

}

void AutoTrader::cancelOrder(unsigned long id, bool ask) {

    RLOG(LG_AT, LogLevel::LL_INFO) << "Cancelling order: " << id;

    SendCancelOrder(id);
    if (ask) {
        mAskId = 0;
        mAskPrice = 0;
    } else {
        mBidPrice = 0;
        mBidPrice = 0;
    }
}

void AutoTrader::setUpAwaitingCancelOrder(unsigned long id, unsigned long price, bool ask) {
    if (ask) {
        mMakeAskAwaitingCancelId = id;
        mMakeAskAwaitingCancelPrice = price;

    } else {
        mMakeBidAwaitingCancelId = id;
        mMakeBidAwaitingCancelPrice = price;
    }
}


void AutoTrader::OrderFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "order " << clientOrderId << " filled for " << volume
                                   << " lots at $" << price << " cents";
    if (mAsks.count(clientOrderId) == 1)
    {
        etfPosition -= (long)volume;
        SendHedgeOrder(++mNextMessageId, Side::BUY, MAX_ASK_NEAREST_TICK, volume);
    }
    else if (mBids.count(clientOrderId) == 1)
    {
        etfPosition += (long)volume;
        SendHedgeOrder(++mNextMessageId, Side::SELL, MIN_BID_NEARST_TICK, volume);
    }
}

void AutoTrader::OrderStatusMessageHandler(unsigned long clientOrderId,
                                           unsigned long fillVolume,
                                           unsigned long remainingVolume,
                                           signed long fees)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "Something happened to order: " << clientOrderId;

    // Check if it is a cancelled trade that now needs ot place a new trade
    if (clientOrderId == mMakeAskAwaitingCancelId) replaceCancelledTrade(true, remainingVolume);
    else if (clientOrderId == mMakeBidAwaitingCancelId) replaceCancelledTrade(false, remainingVolume);

    if (!remainingVolume)
    {
        if (clientOrderId == mAskId)
        {
            mAskId = 0;
        }
        else if (clientOrderId == mBidId)
        {
            mBidId = 0;
        }

        mAsks.erase(clientOrderId);
        mBids.erase(clientOrderId);
    }
}

void AutoTrader::TradeTicksMessageHandler(Instrument instrument,
                                          unsigned long sequenceNumber,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
    // RLOG(LG_AT, LogLevel::LL_INFO) << "trade ticks received for " << instrument << " instrument"
    //                                << ": ask prices: " << askPrices[0]
    //                                << "; ask volumes: " << askVolumes[0]
    //                                << "; bid prices: " << bidPrices[0]
    //                                << "; bid volumes: " << bidVolumes[0];

    // RLOG(LG_AT, LogLevel::LL_INFO) << "Trade tick: " << ticks++;

}