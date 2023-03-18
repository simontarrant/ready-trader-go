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

using namespace ReadyTraderGo;

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

constexpr unsigned long POSITION_LIMIT = 100;
constexpr int TICK_SIZE_IN_CENTS = 100;
constexpr int BID_ASK_CLEARANCE = 1 * TICK_SIZE_IN_CENTS;
constexpr int FUT_CLEARANCE = 1 * TICK_SIZE_IN_CENTS;
// constexpr int FUT_CLEARANCE_PAIR = 1 * TICK_SIZE_IN_CENTS;
constexpr int MIN_BID_NEARST_TICK = (MINIMUM_BID + TICK_SIZE_IN_CENTS) / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int MAX_ASK_NEAREST_TICK = MAXIMUM_ASK / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;

AutoTrader::AutoTrader(boost::asio::io_context& context) : BaseAutoTrader(context)
{
}

void AutoTrader::DisconnectHandler()
{
    BaseAutoTrader::DisconnectHandler();
    // RLOG(LG_AT, LogLevel::LL_INFO) << "execution connection lost";
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

        // There are futures asks
        if (askPrices[0]) {
            // If we have an ask
            if (mAskId) {
                // If ask is not at ideal price
                if (mAskPrice != askPrices[0] + FUT_CLEARANCE) {
                    SendCancelOrder(mAskId);
                    makeAskBasedOnFut(askPrices[0]);
                }
            }
            // If we dont have an ask -> make a new one
            else {
                makeAskBasedOnFut(askPrices[0]);
            }
        }

        // If there are futures bids
        if (bidPrices[0]) {
            // if we have a current bid
            if (mBidId) {
                // If current bid is not in optimal spot -> cancel and make new bid
                if (mBidPrice != bidPrices[0] - FUT_CLEARANCE) {
                    SendCancelOrder(mBidId);
                    makeBidBasedOnFut(bidPrices[0]);
                }
            }
            // We have no curr bid -> create a new one
            else {
                makeBidBasedOnFut(bidPrices[0]);
            }
        }

        // Copy in futures values to be used when etf info comes through
        // futAskPrice = askPrices[0];
        // futBidPrice = bidPrices[0];
    }
    // Etf order book info -> see if any pairs trading opportunities exist, will this be too slow??
    // else {
    //     // if futures prices exist, attempt pair trade, if no trade to be made, FAK orders will simply not fill
    //     if (futAskPrice) makePairsAsk();
    //     if (futBidPrice) makePairsBid();
    // }
}

void AutoTrader::makeAskBasedOnFut(unsigned long futBestAskPrice) {
    unsigned long makeAskVol = maxAskVol();
    if (makeAskVol) {

        mAskPrice = futBestAskPrice + FUT_CLEARANCE;
        mAskId = ++mNextMessageId;

        SendInsertOrder(mAskId, Side::SELL, mAskPrice, makeAskVol, Lifespan::GOOD_FOR_DAY);
        mAsks.insert(mAskId);
    }
}

void AutoTrader::makeBidBasedOnFut(unsigned long futBestBidPrice) {
    unsigned long makeBidVol = maxBidVol();
    if (makeBidVol) {

        mBidPrice = futBestBidPrice - FUT_CLEARANCE;
        mBidId = ++mNextMessageId;

        SendInsertOrder(mBidId, Side::BUY, mBidPrice, makeBidVol, Lifespan::GOOD_FOR_DAY);
        mBids.insert(mBidId);
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