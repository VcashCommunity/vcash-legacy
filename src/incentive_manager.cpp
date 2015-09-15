/*
 * Copyright (c) 2013-2015 John Connor (BM-NC49AxAjcqVcF5jNPu85Rb8MJ2d9JqZt)
 *
 * This file is part of vanillacoin.
 *
 * vanillacoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <coin/address.hpp>
#include <coin/database_stack.hpp>
#include <coin/destination.hpp>
#include <coin/globals.hpp>
#include <coin/incentive.hpp>
#include <coin/incentive_manager.hpp>
#include <coin/key.hpp>
#include <coin/logger.hpp>
#include <coin/message.hpp>
#include <coin/script.hpp>
#include <coin/stack_impl.hpp>
#include <coin/tcp_connection.hpp>
#include <coin/tcp_connection_manager.hpp>
#include <coin/types.hpp>
#include <coin/wallet.hpp>

using namespace coin;

incentive_manager::incentive_manager(
    boost::asio::io_service & ios, boost::asio::strand & s,
    stack_impl & owner
    )
    : m_collateral_is_valid(false)
    , m_collateral_balance(0.0f)
    , io_service_(ios)
    , strand_(s)
    , stack_impl_(owner)
    , timer_(ios)
    , timer_check_inputs_(ios)
    , last_block_height_(0)
{
    // ...
}

void incentive_manager::start()
{
    if (globals::instance().is_incentive_enabled())
    {
        log_debug("Incentive manager is starting.");
        
        /**
         * Start the timer.
         */
        do_tick(8);
        
        /**
         * Start the check inputs timer.
         */
        do_tick_check_inputs(12);
    }
}

void incentive_manager::stop()
{
    log_debug("Incentive manager is stopping.");
    
    timer_.cancel();
    timer_check_inputs_.cancel();
}

bool incentive_manager::handle_message(
    const boost::asio::ip::tcp::endpoint & ep, message & msg
    )
{
    if (globals::instance().is_incentive_enabled())
    {
        if (msg.header().command == "ivote")
        {
            if (msg.protocol_ivote().ivote->score() > -1)
            {
                log_debug(
                    "Incentive manager got vote for " <<
                    msg.protocol_ivote().ivote->block_height() + 2 << ":" <<
                    msg.protocol_ivote().ivote->address().substr(0, 8) << "."
                );
                
                std::lock_guard<std::mutex> l1(mutex_votes_);
                
                votes_[msg.protocol_ivote().ivote->block_height() + 2
                    ][msg.protocol_ivote().ivote->address()].push_back(
                    *msg.protocol_ivote().ivote
                );

                auto incentive_votes = votes_[
                    msg.protocol_ivote().ivote->block_height() + 2
                ];
                
                std::stringstream ss;
                
                ss << "votes:\n";
                
                auto index = 0;
                
                std::size_t most_votes = 0;
                
                std::string winner;
                
                for (auto & i : incentive_votes)
                {
                    ++index;
                    
                    if (i.second.size() > most_votes)
                    {
                        most_votes = i.second.size();
                        
                        winner = i.first;
                    }
                    
                    ss <<
                        "\t" << index << ". " <<
                        i.first.substr(0, 8) << ":" <<
                        i.second.size() << "\n"
                    ;
                }
                
                log_debug(ss.str());
                
                /**
                 * The number of votes required to qualify.
                 */
                enum { minimum_votes = 8 };

                /**
                 * Check if they won.
                 */
                if (most_votes >= minimum_votes)
                {
                    log_debug(
                        "Incentive manager got winner " <<
                        winner.substr(0, 8) << " for block " <<
                        msg.protocol_ivote().ivote->block_height() + 2 << "."
                    );
                
                    /**
                     * Set the winner so far, as votes are counted the winner
                     * could change.
                     */
                    incentive::instance().winners()[
                        msg.protocol_ivote().ivote->block_height() + 2] = winner
                    ;
                }

                ss.clear();
                
                ss << "all votes:\n";
                
                for (auto & i : votes_)
                {
                    for (auto & j : i.second)
                    {
                        std::string addr;
                        auto votes = 0;
                    
                        addr = j.first.substr(0, 8);
                        votes += j.second.size();
                        
                        ss <<
                            "\t" << i.first << ". " << addr << ":" <<
                            votes << "\n"
                        ;
                    }
                }
                
                log_debug(ss.str());
            }
        }
        else
        {
            return false;
        }
    }
    else
    {
        return false;
    }
    
    return true;
}

const double & incentive_manager::collateral_balance() const
{
    return m_collateral_balance;
}

void incentive_manager::do_tick(const std::uint32_t & interval)
{
    auto self(shared_from_this());
    
    timer_.expires_from_now(std::chrono::seconds(interval));
    timer_.async_wait(strand_.wrap([this, self, interval]
        (boost::system::error_code ec)
    {
        if (ec)
        {
            // ...
        }
        else
        {
            if (globals::instance().is_incentive_enabled())
            {
                if (incentive::instance().get_key().is_null())
                {
                    log_debug("Incentive manager key is null, trying wallet.");
                    
                    if (globals::instance().wallet_main()->is_locked())
                    {
                        log_debug(
                            "Incentive manager wallet is locked, will try "
                            "again."
                        );
                    }
                    else
                    {
                        types::id_key_t key_id;
                        
                        address addr(globals::instance().wallet_main(
                            )->key_public_default().get_id()
                        );
                        
                        if (addr.get_id_key(key_id))
                        {
                            key k;
                        
                            if (
                                globals::instance().wallet_main()->get_key(
                                key_id, k)
                                )
                            {
                                log_debug(
                                    "Incentive manager is setting key to " <<
                                    addr.to_string() << "."
                                );
                                
                                incentive::instance().set_key(k);
                            }
                        }
                        else
                        {
                            log_error("Incentive manager failed to get key.");
                        }
                    }
                }
                else
                {
                    log_debug("Incentive manager key is set.");
                }
                
                if (incentive::instance().get_key().is_null() == false)
                {
                    /**
                     * Get our best block height.
                     */
                    auto block_height =
                        globals::instance().best_block_height()
                    ;
                
                    /**
                     *  Get the block index.
                     */
                    auto index =
                        utility::find_block_index_by_height(block_height)
                    ;
                
                    /**
                     * Check if the block height has changed.
                     */
                    if (block_height > last_block_height_)
                    {
                        last_block_height_ = block_height;
                        
                        /**
                         * Get the block height to vote for.
                         */
                        auto vote_block_height = block_height + 2;
                        
                        /**
                         * Remove winners older than 4 blocks.
                         */
                        auto it1 = incentive::instance().winners().begin();
                        
                        while (it1 != incentive::instance().winners().end())
                        {
                            if (vote_block_height - it1->first > 4)
                            {
                                it1 =
                                    incentive::instance().winners().erase(it1)
                                ;
                            }
                            else
                            {
                                ++it1;
                            }
                        }
                        
                        /**
                         * Remove votes older than 4 blocks.
                         */
                        auto it2 = incentive::instance().votes().begin();
                        
                        while (it2 != incentive::instance().votes().end())
                        {
                            if (
                                vote_block_height -
                                it2->second.block_height() > 4
                                )
                            {
                                it2 = incentive::instance().votes().erase(it2);
                            }
                            else
                            {
                                ++it2;
                            }
                        }
                        
                        /**
                         * Remove candidates older than 20 mins.
                         */
                        std::lock_guard<std::mutex> l1(mutex_candidates_);
                    
                        auto it3 = candidates_.begin();
                        
                        while (it3 != candidates_.end())
                        {
                            if (std::time(0) - it3->second.first > 20 * 60)
                            {
                                it3 = candidates_.erase(it3);
                            }
                            else
                            {
                                ++it3;
                            }
                        }
                        
                        /**
                         * Remove votes older than 4 blocks.
                         */
                        std::lock_guard<std::mutex> l2(mutex_votes_);
                        
                        auto it4 = votes_.begin();
                        
                        while (it4 != votes_.end())
                        {
                            if (vote_block_height - it4->first > 4)
                            {
                                it4 = votes_.erase(it4);
                            }
                            else
                            {
                                ++it4;
                            }
                        }
                        
                        /**
                         * Get the recent good endpoints.
                         */
                        auto recent_good_endpoints =
                            stack_impl_.get_address_manager(
                            )->recent_good_endpoints()
                        ;
      
                        /**
                         * Get the K closest nodes to the vote block height.
                         */
                        auto kclosest = k_closest(
                            recent_good_endpoints, vote_block_height
                        );

                        /**
                         * The winner.
                         */
                        address_manager::recent_endpoint_t winner;
                        
                        /**
                         * Using the rate limit is not a network consensus
                         * and used for testing purposes only.
                         */
                        auto use_time_rate_limit = false;
                        
                        if (kclosest.size() >= 2)
                        {
                            log_debug(
                                "kclosest0: " << vote_block_height <<
                                ":" << kclosest[0].addr.ipv4_mapped_address(
                                ).to_string().substr(0, 8) <<
                                ":" << kclosest[0].addr.port
                            );
                            log_debug(
                                "kclosest1: " << vote_block_height <<
                                ":" << kclosest[1].addr.ipv4_mapped_address(
                                ).to_string().substr(0, 8) <<
                                ":" << kclosest[1].addr.port
                            );
                            
                            if ((vote_block_height & 1) == 0)
                            {
                                log_debug(
                                    "candidate: " << vote_block_height << ":" <<
                                    kclosest[0].addr.ipv4_mapped_address(
                                    ).to_string().substr(0, 8) <<
                                    ":" << kclosest[0].addr.port
                                );
                                
                                if (
                                    use_time_rate_limit ? std::time(0) -
                                    candidates_[kclosest[0]].first >
                                    1 * 60 * 60 : true
                                    )
                                {
                                    winner = kclosest[0];
                                }
                                else
                                {
                                    log_debug(
                                        "Candidate " <<
                                        kclosest[0].addr.ipv4_mapped_address(
                                        ).to_string().substr(0, 8) <<
                                        ":" << kclosest[0].addr.port <<
                                        " too soon."
                                    );
                                    
                                    /**
                                     * Try the other candidate otherwise for
                                     * the next closest node that we know has
                                     * not been a recent candidate.
                                     */
                                    if (
                                        use_time_rate_limit ?
                                        std::time(0) -
                                        candidates_[kclosest[1]].first >
                                        1 * 60 * 60 : true
                                        )
                                    {
                                        winner = kclosest[1];
                                    }
                                    else
                                    {
                                        /**
                                         * The top two were aready candidates
                                         * in the past hour, use the next
                                         * closest node that has not recently
                                         * been a candidate.
                                         */
                                        for (auto & i : kclosest)
                                        {
                                            if (
                                                candidates_.count(i) > 0
                                                && (use_time_rate_limit ?
                                                std::time(0) -
                                                candidates_[i].first >
                                                1 * 60 * 60 : true)
                                                )
                                            {
                                                winner = i;
                                                
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                            else
                            {
                                log_debug(
                                    "candidate: " << vote_block_height << ":" <<
                                    kclosest[1].addr.ipv4_mapped_address(
                                    ).to_string().substr(0, 8) <<
                                    ":" << kclosest[1].addr.port
                                );
                                
                                if (
                                    use_time_rate_limit ? std::time(0) -
                                    candidates_[kclosest[1]].first >
                                    1 * 60 * 60 : true
                                    )
                                {
                                    winner = kclosest[1];
                                }
                                else
                                {
                                    log_debug(
                                        "Candidate " <<
                                        kclosest[1
                                        ].addr.ipv4_mapped_address(
                                        ).to_string().substr(0, 8) << ":" <<
                                        kclosest[1].addr.port << " too soon."
                                    );
                                    
                                    /**
                                     * Try the other candidate otherwise for
                                     * the next closest node that we know has
                                     * not been a recent candidate.
                                     */
                                    if (
                                        use_time_rate_limit ?
                                        std::time(0) -
                                        candidates_[kclosest[0]].first >
                                        1 * 60 * 60 : true
                                        )
                                    {
                                        winner = kclosest[0];
                                    }
                                    else
                                    {
                                        /**
                                         * The top two were aready candidates
                                         * in the past hour, use the next
                                         * closest node that has not recently
                                         * been a candidate.
                                         */
                                        for (auto & i : kclosest)
                                        {
                                            if (
                                                candidates_.count(i) > 0
                                                && (use_time_rate_limit ?
                                                std::time(0) -
                                                candidates_[i].first >
                                                1 * 60 * 60 : true)
                                                )
                                            {
                                                winner = i;
                                                
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                            
                            std::stringstream ss;
                            
                            ss << "candidate_stats:\n";
                            
                            auto index = 0;
                            auto sum = 0;
                            
                            for (auto & i : candidates_)
                            {
                                ss <<
                                    "\t" << ++index << ". " <<
                                    i.first.addr.ipv4_mapped_address(
                                    ).to_string().substr(0, 8) << ":" <<
                                    i.first.addr.port << ":" <<
                                    i.first.wallet_address.substr(0, 8
                                    ) << ":" << i.second.second << "\n"
                                ;
                                
                                sum += i.second.second;
                            }
                            
                            log_debug(ss.str());
                            log_debug("sum of all candidates = " << sum);

                            if (winner.wallet_address.size() > 0)
                            {
                                /**
                                 * Cast vote.
                                 */
                                vote(winner.wallet_address);

                                candidates_[winner].first =
                                    std::time(0)
                                ;
                                candidates_[winner].second++;
                            }
                        }
                    }
                }

                /**
                 * Start the timer.
                 */
                do_tick(8);
            }
        }
    }));
}

void incentive_manager::do_tick_check_inputs(const std::uint32_t & interval)
{
    auto self(shared_from_this());
    
    timer_check_inputs_.expires_from_now(std::chrono::seconds(interval));
    timer_check_inputs_.async_wait(strand_.wrap([this, self, interval]
        (boost::system::error_code ec)
    {
        if (ec)
        {
            // ...
        }
        else
        {
            if (incentive::instance().collateral > 0)
            {
                if (incentive::instance().get_key().is_null() == false)
                {
                    /**
                     * Check that the collateral is valid.
                     */
                    try
                    {
                        address addr(
                            incentive::instance().get_key(
                            ).get_public_key().get_id()
                        );

                        script script_collateral;
                        
                        script_collateral.set_destination(addr.get());

                        transaction tx;
                        
                        transaction_out vout = transaction_out(
                            incentive::collateral * constants::coin,
                            script_collateral
                        );
                        tx.transactions_in().push_back(
                            incentive::instance().get_transaction_in()
                        );
                        tx.transactions_out().push_back(vout);

                        if (
                            transaction_pool::instance().acceptable(
                            tx).first == false
                            )
                        {
                            log_error(
                                "Incentive manager detected spent "
                                "collateral, will keep looking."
                            );
                            
                            m_collateral_is_valid = false;
                        }
                        else
                        {
                            log_debug(
                                "Incentive manager detected valid "
                                "collateral."
                            );
                            
                            m_collateral_is_valid = true;
                        }
                    }
                    catch (std::exception & e)
                    {
                        log_error(
                            "Incentive manager detected invalid collateral, "
                            "what = " << e.what() << "."
                        );
                        
                        m_collateral_is_valid = false;
                    }
                    
                    /**
                     * If the collateral is not valid let's try to find some.
                     */
                    if (m_collateral_is_valid == false)
                    {
                        /**
                         * Get candidate coins.
                         */
                        auto coins = select_coins();
                        
                        /**
                         * Allocate the transaction_in.
                         */
                        transaction_in tx_in;

                        /**
                         * Get the incentive public key.
                         */
                        auto public_key =
                            incentive::instance().get_key().get_public_key()
                        ;

                        /**
                         * Check the coins for valid collateral stopping at
                         * the first valid input.
                         */
                        for (auto & i : coins)
                        {
                            auto * output_ptr = &i;
                            
                            if (output_ptr)
                            {
                                if (
                                    tx_in_from_output(*output_ptr, tx_in,
                                    public_key, incentive::instance().get_key())
                                    )
                                {
                                    log_debug(
                                        "Incentive manager got tx_in = " <<
                                        tx_in.to_string() << "."
                                    );

                                    /**
                                     * Check if the collateral is spendable.
                                     */
                                    address addr(
                                        incentive::instance().get_key(
                                        ).get_public_key().get_id()
                                    );

                                    script script_collateral;
                                    
                                    script_collateral.set_destination(
                                        addr.get()
                                    );

                                    transaction tx;
                                    
                                    transaction_out vout = transaction_out(
                                        incentive::collateral * constants::coin,
                                        script_collateral
                                    );
                                    tx.transactions_in().push_back(tx_in);
                                    tx.transactions_out().push_back(vout);
                    
                                    if (
                                        transaction_pool::instance(
                                        ).acceptable(tx).first
                                        )
                                    {
                                        log_debug(
                                            "Incentive manager found valid "
                                            "collateral input " <<
                                            tx_in.to_string() << "."
                                        );

                                        incentive::instance(
                                            ).set_transaction_in(tx_in
                                        );
                                        
                                        m_collateral_balance =
                                           static_cast<double> (
                                           i.get_transaction_wallet(
                                           ).transactions_out()[i.get_i()
                                           ].value()) / constants::coin
                                        ;
                                        
                                        m_collateral_is_valid = true;
                                        
                                        break;
                                    }
                                    else
                                    {
                                        log_debug(
                                            "Incentive manager found invalid "
                                            "collateral input, checking more."
                                        );
                                        
                                        incentive::instance(
                                            ).set_transaction_in(
                                            transaction_in()
                                        );
                                        
                                        m_collateral_is_valid = false;
                                    }
                                }
                                else
                                {
                                    log_error(
                                        "Incentive manager failed to "
                                        "tx_in_from_output."
                                    );
                                    
                                    incentive::instance().set_transaction_in(
                                        transaction_in()
                                    );
                                    
                                    m_collateral_is_valid = false;
                                }
                            }
                        }
                    }
                }
                else
                {
                    log_error(
                        "Incentive manager failed to find collateral input, "
                        "wallet is locked."
                    );
                }
                
                /**
                 * Start the check inputs timer.
                 */
                do_tick_check_inputs(10 * 60);
            }
        }
    }));
}

bool incentive_manager::vote(const std::string & wallet_address)
{
    /**
     * Get the best block index.
     */
    auto index =
        utility::find_block_index_by_height(
        globals::instance().best_block_height()
    );

    if (index && incentive::instance().get_key().is_null() == false)
    {
        /**
         * Allocate the data_buffer.
         */
        data_buffer buffer;
        
        /**
         * Allocate the incentive_vote.
         */
        incentive_vote ivote(
            index->height(),
            index->get_block_hash(), wallet_address,
            incentive::instance().get_key(
            ).get_public_key()
        );
        
        /**
         * Encode the incentive_vote.
         */
        ivote.encode(buffer);

        /**
         * Calulate our vote score.
         */
        const auto & vote_score = ivote.score();
        
        log_debug(
            "Incentve manager forming vote, "
            "calculated score = " << vote_score <<
            " for " <<
            ivote.address().substr(0, 8) << "."
        );
        
        /**
         * If our vote score is at least zero we
         * can vote otherwise peers will reject it.
         */
        if (vote_score > -1)
        {
            if (utility::is_initial_block_download() == false)
            {
                /**
                 * Allocate the inventory_vector.
                 */
                inventory_vector inv(
                    inventory_vector::type_msg_ivote, ivote.hash_nonce()
                );
                
                /**
                 * Insert the incentive_vote.
                 */
                incentive::instance().votes()[ivote.hash_nonce()] = ivote;

                /**
                 * Get the TCP connections
                 */
                auto tcp_connections =
                    stack_impl_.get_tcp_connection_manager(
                    )->tcp_connections()
                ;
                
                for (auto & i : tcp_connections)
                {
                    if (auto t = i.second.lock())
                    {
                        t->send_relayed_inv_message(inv, buffer);
                    }
                }

                /**
                 * Allocate the message.
                 */
                message msg(inv.command(), buffer);

                /**
                 * Encode the message.
                 */
                msg.encode();

                /**
                 * Allocate the UDP packet.
                 */
                std::vector<std::uint8_t> udp_packet(msg.size());
                
                /**
                 * Copy the message to the UDP packet.
                 */
                std::memcpy(&udp_packet[0], msg.data(), msg.size());
        
                /**
                 * Broadcast the message over UDP.
                 */
                stack_impl_.get_database_stack()->broadcast(udp_packet);
            }
        }
        else
        {
            return false;
        }
    }
    
    return true;
}

std::vector<address_manager::recent_endpoint_t> incentive_manager::k_closest(
    const std::vector<address_manager::recent_endpoint_t> & nodes,
    const std::uint32_t & block_height, const std::uint32_t & k
    )
{
    std::vector<address_manager::recent_endpoint_t> ret;
    
    std::map<std::uint32_t, address_manager::recent_endpoint_t> entries;
    
    /**
     * Sort all nodes by distance.
     */
    for (auto & i : nodes)
    {
        if (
            i.addr.ipv4_mapped_address().is_loopback() ||
            i.addr.ipv4_mapped_address().is_multicast() ||
            i.addr.ipv4_mapped_address().is_unspecified())
        {
            continue;
        }
        else
        {
            auto distance =
                block_height ^ incentive::instance().calculate_score(
                boost::asio::ip::tcp::endpoint(i.addr.ipv4_mapped_address(),
                i.addr.port)
            );

            entries.insert(std::make_pair(distance, i));
        }
    }
    
    /**
     * Limit the number of votes to K.
     */
    for (auto & i : entries)
    {
        ret.push_back(i.second);
        
        if (ret.size() >= k)
        {
            break;
        }
    }
    
    return ret;
}

std::vector<output> incentive_manager::select_coins()
{
    std::vector<output> ret;
    
    std::vector<output> coins;
    
    globals::instance().wallet_main()->available_coins(coins, true, 0);

    for (auto & i : coins)
    {
        if (
            i.get_transaction_wallet().transactions_out()[
            i.get_i()].value() >= incentive::collateral * constants::coin
            )
        {
            ret.push_back(i);
        }
    }
    
    return ret;
}

bool incentive_manager::tx_in_from_output(
    const output & out, transaction_in & tx_in, key_public & public_key,
    key & k
    )
{
    tx_in =
        transaction_in(out.get_transaction_wallet().get_hash(), out.get_i())
    ;
    
    auto script_public_key =
        out.get_transaction_wallet().transactions_out()[
        out.get_i()].script_public_key()
    ;

    destination::tx_t dest_tx;

    if (script::extract_destination(script_public_key, dest_tx) == false)
    {
        log_error(
            "Incentive manager failed to get tx_in, unable to extract "
            "destination."
        );
        
        return false;
    }
    
    address addr(dest_tx);

    /**
     * The coins must be in the default wallet address.
     */
    if (
        address(incentive::instance().get_key().get_public_key(
        ).get_id()).to_string() != addr.to_string()
        )
    {
        log_error(
            "Incentive manager failed to get tx_in, address is not "
            "the default."
        );
        
        return false;
    }

    types::id_key_t key_id;
    
    if (addr.get_id_key(key_id) == false)
    {
        log_error(
            "Incentive manager failed to get tx_in, address does not "
            "match key."
        );
        
        return false;
    }

    if (globals::instance().wallet_main()->get_key(key_id, k) == false)
    {
        log_error(
            "Incentive manager failed to get tx_in, unknown private key."
        );
        
        return false;
    }

    public_key = k.get_public_key();

    return true;
}
