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

using namespace ReadyTraderGo;

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

constexpr int LOT_SIZE = 10;
constexpr int POSITION_LIMIT = 100;
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
void AutoTrader::OrderBookMessageHandler(Instrument instrument,
                                         unsigned long sequenceNumber,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
    if (instrument == Instrument::FUTURE) {
        // RLOG(LG_AT, LogLevel::LL_INFO) << "Futures order update: " << orderBookTicks++ << " "
        //                                 << askPrices[0] << ", "
        //                                 << askVolumes[0] << ", "
        //                                 << bidPrices[0] << ", "
        //                                 << bidVolumes[0];

        // Remember futures prices for pairwise trading when message for etf comes through
        std::copy(askPrices.begin(), askPrices.end(), prevAskPrices.begin());
        std::copy(askVolumes.begin(), askVolumes.end(), prevAskVolumes.begin());
        std::copy(bidPrices.begin(), bidPrices.end(), prevBidPrices.begin());
        std::copy(bidVolumes.begin(), bidVolumes.end(), prevBidVolumes.begin());

    }
    // Etf message
    else {
        // RLOG(LG_AT, LogLevel::LL_INFO) << "ETF order update: " << orderBookTicks++ << " "
        //                                 << prevAskPrices[0] << ", "
        //                                 << prevAskVolumes[0] << ", "
        //                                 << prevBidPrices[0] << ", "
        //                                 << prevBidVolumes[0];

        // Pairwise opportunity, ETF bid > FUT ask => buy futures, sell ETF
        if (bidPrices[0] && bidPrices[0] > prevAskPrices[0]) {
            RLOG(LG_AT, LogLevel::LL_INFO) << "Pairwise opportunity, ETF > FUT => buy futures, sell ETF";

            // Find the volume and price we are willing to execute at for fill and kill order
            int etfIdx = 0;
            int futIdx = 0;
            int sellVol = 0;
            while (etfIdx < ReadyTraderGo::TOP_LEVEL_COUNT && futIdx < ReadyTraderGo::TOP_LEVEL_COUNT &&
                    bidPrices[etfIdx] > prevAskPrices[futIdx]) {

                // added volume will be min of either what the position limit will allow, or the smaller volume at this price level
                int minVolume;
                int smaller;
                if (bidVolumes[etfIdx] < prevAskVolumes[futIdx]) {
                    minVolume = bidVolumes[etfIdx];
                    smaller = 0;
                } else if (bidVolumes[etfIdx] > prevAskVolumes[futIdx]) {
                    minVolume = prevAskVolumes[futIdx];
                    smaller = 1;
                } else {
                    minVolume = bidVolumes[etfIdx];
                    smaller = 2;
                }

                int addedVol;
                if (minVolume <= POSITION_LIMIT + etfPosition - sellVol) addedVol = minVolume;
                else addedVol = POSITION_LIMIT + etfPosition - sellVol;

                sellVol += addedVol;
                if (sellVol - etfPosition >= POSITION_LIMIT) break;

                switch (smaller)
                {
                case 0:
                    etfIdx++;
                    break;
                case 1:
                    futIdx++;
                    break;
                default:
                    etfIdx++;
                    futIdx++;
                    break;
                }
            }

            if (sellVol > 0) {
                mAskId = mNextMessageId++;
                RLOG(LG_AT, LogLevel::LL_INFO) << "Sell vol: " << sellVol << " order: " << mAskId;
                SendInsertOrder(mAskId, Side::SELL, bidPrices[0], sellVol, Lifespan::FILL_AND_KILL);
                mAsks.insert(mAskId);
            }
            


        }
        // Pairwise opportunity, FUT bid > ETF ask => buy ETF, sell futures
        else if (askPrices[0] && askPrices[0] < prevBidPrices[0]) {
            RLOG(LG_AT, LogLevel::LL_INFO) << "Pairwise opportunity, FUT > ETF => buy ETF, sell futures";

            const int etfBuyVol = (POSITION_LIMIT - etfPosition <= askVolumes[0])
                                    ? POSITION_LIMIT - etfPosition : askVolumes[0];
            
            if (etfBuyVol > 0) {
                mBidId = mNextMessageId++;
                RLOG(LG_AT, LogLevel::LL_INFO) << "Buy vol: " << etfBuyVol << " order: " << mBidId;
                SendInsertOrder(mBidId, Side::BUY, askPrices[0], etfBuyVol, Lifespan::FILL_AND_KILL);
                mBids.insert(mBidId);
            }
        }
    }


    // RLOG(LG_AT, LogLevel::LL_INFO) << "order book received for " << instrument << " instrument"
    //                                << ": ask prices: " << askPrices[0]
    //                                << "; ask volumes: " << askVolumes[0]
    //                                << "; bid prices: " << bidPrices[0]
    //                                << "; bid volumes: " << bidVolumes[0];


    // if (instrument == Instrument::FUTURE)
    // {
    //     unsigned long priceAdjustment = - (mPosition / LOT_SIZE) * TICK_SIZE_IN_CENTS;
    //     unsigned long newAskPrice = (askPrices[0] != 0) ? askPrices[0] + priceAdjustment : 0;
    //     unsigned long newBidPrice = (bidPrices[0] != 0) ? bidPrices[0] + priceAdjustment : 0;

    //     if (mAskId != 0 && newAskPrice != 0 && newAskPrice != mAskPrice)
    //     {
    //         SendCancelOrder(mAskId);
    //         mAskId = 0;
    //     }
    //     if (mBidId != 0 && newBidPrice != 0 && newBidPrice != mBidPrice)
    //     {
    //         SendCancelOrder(mBidId);
    //         mBidId = 0;
    //     }

    //     if (mAskId == 0 && newAskPrice != 0 && mPosition > -POSITION_LIMIT)
    //     {
    //         mAskId = mNextMessageId++;
    //         mAskPrice = newAskPrice;
    //         SendInsertOrder(mAskId, Side::SELL, newAskPrice, LOT_SIZE, Lifespan::GOOD_FOR_DAY);
    //         mAsks.emplace(mAskId);
    //     }
    //     if (mBidId == 0 && newBidPrice != 0 && mPosition < POSITION_LIMIT)
    //     {
    //         mBidId = mNextMessageId++;
    //         mBidPrice = newBidPrice;
    //         SendInsertOrder(mBidId, Side::BUY, newBidPrice, LOT_SIZE, Lifespan::GOOD_FOR_DAY);
    //         mBids.emplace(mBidId);
    //     }
    // }
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
        SendHedgeOrder(mNextMessageId++, Side::BUY, MAX_ASK_NEAREST_TICK, volume);
    }
    else if (mBids.count(clientOrderId) == 1)
    {
        etfPosition += (long)volume;
        SendHedgeOrder(mNextMessageId++, Side::SELL, MIN_BID_NEARST_TICK, volume);
    }
}

void AutoTrader::OrderStatusMessageHandler(unsigned long clientOrderId,
                                           unsigned long fillVolume,
                                           unsigned long remainingVolume,
                                           signed long fees)
{
    if (remainingVolume == 0)
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

        RLOG(LG_AT, LogLevel::LL_INFO) << "Trade tick: " << ticks++;

}
