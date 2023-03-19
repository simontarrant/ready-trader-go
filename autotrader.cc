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

// Maybe hedge when we hit max either side to stop prolonged fall causing bad directional exposure when there iss lots of momentum

#include <array>

#include <boost/asio/io_context.hpp>

#include <ready_trader_go/logging.h>

#include "autotrader.h"
#include <iostream>

using namespace ReadyTraderGo;

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

constexpr unsigned long POSITION_LIMIT = 100;
constexpr int TICK_SIZE_IN_CENTS = 100;
constexpr int BID_ASK_CLEARANCE = 1 * TICK_SIZE_IN_CENTS;
constexpr int FUT_CLEARANCE = 0 * TICK_SIZE_IN_CENTS;
// constexpr int FUT_CLEARANCE_PAIR = 1 * TICK_SIZE_IN_CENTS;
constexpr int MIN_BID_NEARST_TICK = (MINIMUM_BID + TICK_SIZE_IN_CENTS) / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int MAX_ASK_NEAREST_TICK = MAXIMUM_ASK / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int TICKS_PER_SECOND = 4;
constexpr int MAX_UNHEDGED_SEC = 55;
constexpr int MAX_UNHEDGED_TICKS = MAX_UNHEDGED_SEC * TICKS_PER_SECOND;
constexpr int HEDGE_LIMIT = 10;
// constexpr std::string CROSS_ORDER_MESSAGE = "order rejected: in cross with an existing order";


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
    // RLOG(LG_AT, LogLevel::LL_INFO) << "error with order " << clientOrderId << ": " << errorMessage;


    if (clientOrderId != 0 && ((mAsks.count(clientOrderId) == 1) || (mBids.count(clientOrderId) == 1)))
    {
        OrderStatusMessageHandler(clientOrderId, 0, 0, 0);
    }
}

void AutoTrader::HedgeFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    // RLOG(LG_AT, LogLevel::LL_INFO) << "hedge order " << clientOrderId << " filled for " << volume
    //                                << " lots at $" << price << " average price in cents";
    if (clientOrderId == mHedgeAskId) {
        futPosition -= volume;
        mHedgeAskId = 0;
    }

    else if (clientOrderId == mHedgeBidId) {
        futPosition += volume;
        mHedgeBidId = 0;
    }

    else {
        RLOG(LG_AT, LogLevel::LL_INFO) << "Unrecognised hedge order: " << clientOrderId;
    }
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

        RLOG(LG_AT, LogLevel::LL_INFO) << "ASK PRICES: " << askPrices[0] << " BID PRICES: " << bidPrices[0];

        // There are futures asks
        if (askPrices[0]) {
            // If we have an ask
            if (mAskId) {
                // If ask is not at ideal price
                if (mAskPrice != askPrices[0] + FUT_CLEARANCE) {
                    // RLOG(LG_AT, LogLevel::LL_INFO) << "CANCELLING ASK: " << mAskId;
                    mAskCancelId = mAskId;
                    SendCancelOrder(mAskId);
                    makeAskBasedOnFut(askPrices[0]);
                    // RLOG(LG_AT, LogLevel::LL_INFO) << "SENDING ASK: " << mAskId;                    
                }
            }
            // If we dont have an ask -> make a new one
            else {
                makeAskBasedOnFut(askPrices[0]);
                // RLOG(LG_AT, LogLevel::LL_INFO) << "SENDING ASK: " << mAskId;                    
            }
        }

        // If there are futures bids
        if (bidPrices[0]) {
            // if we have a current bid
            if (mBidId) {
                // If current bid is not in optimal spot -> cancel and make new bid
                if (mBidPrice != bidPrices[0] - FUT_CLEARANCE) {
                    
                    // RLOG(LG_AT, LogLevel::LL_INFO) << "CANCELLING BID: " << mBidId;
                    mBidCancelId = mBidId;
                    SendCancelOrder(mBidId);
                    makeBidBasedOnFut(bidPrices[0]);
                    // RLOG(LG_AT, LogLevel::LL_INFO) << "SENDING BID: " << mBidId;
                }
            }
            // We have no curr bid -> create a new one
            else {
                makeBidBasedOnFut(bidPrices[0]);
                // RLOG(LG_AT, LogLevel::LL_INFO) << "SENDING BID: " << mBidId;
            }
        }

        // Copy in futures values to be used when etf info comes through
        // futAskPrice = askPrices[0];
        // futBidPrice = bidPrices[0];

        // RLOG(LG_AT, LogLevel::LL_INFO) << "BID: " << bidPrices[0] << " ASK: " << askPrices[0];
    }

    // ETF order book update
    else {

        // if (ticksUnhedged % 10 == 0) {
        //     RLOG(LG_AT, LogLevel::LL_INFO) << "SECONDS UNHEDGED: " << ticksUnhedged / 4 << "  ETF POS: " << etfPosition << " FUT POS: " << futPosition;
            
        // }

        // Check hedging info - only happens on etf order updates, 4x second
        // If hedge is within limits
        unsigned int unhedgedVol = abs((-etfPosition) - futPosition);
        if (unhedgedVol <= HEDGE_LIMIT) {
            ticksUnhedged = 0;
        }
        // Hedge out of limit
        else {
            // See if we have been unhedged too long
            if (ticksUnhedged > MAX_UNHEDGED_TICKS) {
                int futTargetPosition = -etfPosition;
                int futTargetDiff = futTargetPosition - futPosition;
                // Need to sell hedge to get down to target fut position
                if (futTargetDiff < 0) {
                    // RLOG(LG_AT, LogLevel::LL_INFO) << "HEDGE, SELL VOL: " << -futTargetDiff;
                    SendHedgeOrder(++mNextMessageId, Side::SELL, MIN_BID_NEARST_TICK, -futTargetDiff);
                    mHedgeAskId = mNextMessageId;
                }
                // Need to buy to get up to fut target pos
                else {
                    // RLOG(LG_AT, LogLevel::LL_INFO) << "HEDGE, BUY VOL: " << futTargetDiff;
                    SendHedgeOrder(++mNextMessageId, Side::BUY, MAX_ASK_NEAREST_TICK, futTargetDiff);
                    mHedgeBidId = mNextMessageId;
                }
                ticksUnhedged = 0;
            } else {
                ticksUnhedged++;
            }
        }
    }
}

void AutoTrader::makeAskBasedOnFut(unsigned long futBestAskPrice) {
    unsigned long makeAskVol = maxAskVol();
    if (makeAskVol) {

        mAskPrice = futBestAskPrice + FUT_CLEARANCE;

        SendInsertOrder(++mNextMessageId, Side::SELL, mAskPrice, makeAskVol, Lifespan::GOOD_FOR_DAY);
        mAskId = mNextMessageId;
        mAskVol = makeAskVol;
        mAsks.insert(mAskId);
    }
}

void AutoTrader::makeBidBasedOnFut(unsigned long futBestBidPrice) {
    unsigned long makeBidVol = maxBidVol();
    if (makeBidVol) {

        mBidPrice = futBestBidPrice - FUT_CLEARANCE;

        SendInsertOrder(++mNextMessageId, Side::BUY, mBidPrice, makeBidVol, Lifespan::GOOD_FOR_DAY);
        mBidId = mNextMessageId;
        mBids.insert(mBidId);
        mBidVol = makeBidVol;
    }
}

unsigned long AutoTrader::maxAskVol() {
    return (POSITION_LIMIT + etfPosition) / 2;
}

unsigned long AutoTrader::maxBidVol() {
    return (POSITION_LIMIT - etfPosition) / 2;
}

void AutoTrader::OrderFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    // RLOG(LG_AT, LogLevel::LL_INFO) << "ORDER FILLED: " << clientOrderId << " PRICE: " << price << " VOL: " << volume;
    if (mAsks.count(clientOrderId) == 1)
    {
        etfPosition -= (long)volume;
        // SendHedgeOrder(++mNextMessageId, Side::BUY, MAX_ASK_NEAREST_TICK, volume);
        
        // If this was the previous ask that we attempted to cancel
        if (clientOrderId == mAskCancelId) {
            // If most recent bid was cancelled for being in cross with the ask that just got cancelled/filled -> resend bid
            // if (mBidInCross) {
            //     RLOG(LG_AT, LogLevel::LL_INFO) << "REPLACING CROSSED BID: " << mBidId;               
            //     makeBidBasedOnFut(mBidPrice);
            //     mBidInCross = false;
            // }
            
            // Check if most recent ask has too much volume in case this order was filled when it should have been cancelled
            unsigned long newVol = maxAskVol();
            if (newVol < mAskVol) {
                SendAmendOrder(mAskId, newVol);
                // RLOG(LG_AT, LogLevel::LL_INFO) << "ORDER AMENDED: " << mAskId << " FROM: " << mAskVol << " TO: " << newVol;
                mBidVol = newVol;
            }
        }
    }
    else if (mBids.count(clientOrderId) == 1)
    {
        etfPosition += (long)volume;
        // SendHedgeOrder(++mNextMessageId, Side::SELL, MIN_BID_NEARST_TICK, volume);

        // If this was the prev bid we attempted to cancel
        if (clientOrderId == mBidCancelId) {

            // If most recent ask was cancelled for being in cross with this order that just got cancelled/filled -> resend ask
            // if (mAskInCross) {
            //     RLOG(LG_AT, LogLevel::LL_INFO) << "REPLACING CROSSED ASK: " << mAskId;
            //     makeAskBasedOnFut(mAskPrice);
            //     mAskInCross = false;
            // }

            // Check if most recent bid now has too much volume in case prev bid filled not cancelled
            unsigned long newVol = maxBidVol();
            if (newVol < mBidVol) {
                SendAmendOrder(mBidId, newVol);
                // RLOG(LG_AT, LogLevel::LL_INFO) << "ORDER AMENDED: " << mBidId << " FROM: " << mBidVol << " TO: " << newVol;
                mBidVol = newVol;
            }
        }
    }
    // RLOG(LG_AT, LogLevel::LL_INFO) << "order " << clientOrderId << " filled for " << volume
    //                                << " lots at $" << price << " cents";
}

void AutoTrader::OrderStatusMessageHandler(unsigned long clientOrderId,
                                           unsigned long fillVolume,
                                           unsigned long remainingVolume,
                                           signed long fees)
{
    // RLOG(LG_AT, LogLevel::LL_INFO) << "Order status update: " << clientOrderId;

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