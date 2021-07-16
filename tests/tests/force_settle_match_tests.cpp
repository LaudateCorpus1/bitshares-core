/*
 * Copyright (c) 2021 Abit More, and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <boost/test/unit_test.hpp>

#include <graphene/chain/hardfork.hpp>

#include <graphene/protocol/market.hpp>
#include <graphene/chain/market_object.hpp>

#include "../common/database_fixture.hpp"

using namespace graphene::chain;
using namespace graphene::chain::test;

BOOST_FIXTURE_TEST_SUITE(force_settle_match_tests, database_fixture)

/***
 * BSIP38 "target_collateral_ratio" test after hf core-2481:
 *   matching a taker settle order with multiple maker call orders
 */
BOOST_AUTO_TEST_CASE(tcr_test_hf2481_settle_call)
{ try {

   auto mi = db.get_global_properties().parameters.maintenance_interval;
   generate_blocks(HARDFORK_CORE_2481_TIME - mi);
   generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

   set_expiration( db, trx );

   ACTORS((buyer)(buyer2)(buyer3)(seller)(borrower)(borrower2)(borrower3)(feedproducer));

   const auto& bitusd = create_bitasset("USDBIT", feedproducer_id);
   const auto& core   = asset_id_type()(db);
   asset_id_type usd_id = bitusd.id;

   {
      // set margin call fee ratio
      asset_update_bitasset_operation uop;
      uop.issuer = usd_id(db).issuer;
      uop.asset_to_update = usd_id;
      uop.new_options = usd_id(db).bitasset_data(db).options;
      uop.new_options.extensions.value.margin_call_fee_ratio = 30; // 3%

      trx.clear();
      trx.operations.push_back(uop);
      PUSH_TX(db, trx, ~0);
   }

   int64_t init_balance(1000000);

   transfer(committee_account, buyer_id, asset(init_balance));
   transfer(committee_account, buyer2_id, asset(init_balance));
   transfer(committee_account, buyer3_id, asset(init_balance));
   transfer(committee_account, borrower_id, asset(init_balance));
   transfer(committee_account, borrower2_id, asset(init_balance));
   transfer(committee_account, borrower3_id, asset(init_balance));
   update_feed_producers( bitusd, {feedproducer.id} );

   price_feed current_feed;
   current_feed.maintenance_collateral_ratio = 1750;
   current_feed.maximum_short_squeeze_ratio = 1100;
   current_feed.settlement_price = bitusd.amount( 1 ) / core.amount(5);
   publish_feed( bitusd, feedproducer, current_feed );
   // start out with 300% collateral, call price is 15/1.75 CORE/USD = 60/7, tcr 170% is lower than 175%
   const call_order_object& call = *borrow( borrower, bitusd.amount(1000), asset(15000), 1700);
   call_order_id_type call_id = call.id;
   // create another position with 310% collateral, call price is 15.5/1.75 CORE/USD = 62/7, tcr 200% > 175%
   const call_order_object& call2 = *borrow( borrower2, bitusd.amount(1000), asset(15500), 2000);
   call_order_id_type call2_id = call2.id;
   // create yet another position with 500% collateral, call price is 25/1.75 CORE/USD = 100/7, no tcr
   const call_order_object& call3 = *borrow( borrower3, bitusd.amount(1000), asset(25000));
   transfer(borrower, seller, bitusd.amount(1000));
   transfer(borrower2, seller, bitusd.amount(1000));
   transfer(borrower3, seller, bitusd.amount(1000));

   BOOST_CHECK_EQUAL( 1000, call.debt.value );
   BOOST_CHECK_EQUAL( 15000, call.collateral.value );
   BOOST_CHECK_EQUAL( 1000, call2.debt.value );
   BOOST_CHECK_EQUAL( 15500, call2.collateral.value );
   BOOST_CHECK_EQUAL( 1000, call3.debt.value );
   BOOST_CHECK_EQUAL( 25000, call3.collateral.value );
   BOOST_CHECK_EQUAL( 3000, get_balance(seller, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(seller, core) );
   BOOST_CHECK_EQUAL( 3000, get_balance(seller, bitusd) );
   BOOST_CHECK_EQUAL( init_balance - 15000, get_balance(borrower, core) );
   BOOST_CHECK_EQUAL( init_balance - 15500, get_balance(borrower2, core) );
   BOOST_CHECK_EQUAL( init_balance - 25000, get_balance(borrower3, core) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower2, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower3, bitusd) );

   // adjust price feed to get call and call2 (but not call3) into margin call territory
   current_feed.settlement_price = bitusd.amount( 1 ) / core.amount(10);
   publish_feed( bitusd, feedproducer, current_feed );
   // settlement price = 1/10, mssp = 1/11

   // This sell order above MSSP will not be matched with a call
   limit_order_id_type sell_high = create_sell_order(seller, bitusd.amount(7), core.amount(78))->id;
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( sell_high )->for_sale.value, 7 );

   BOOST_CHECK_EQUAL( 2993, get_balance(seller, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(seller, core) );

   // This buy order is too low will not be matched with a sell order
   limit_order_id_type buy_low = create_sell_order(buyer, asset(80), bitusd.amount(10))->id;
   // This buy order at MSSP will be matched only if no margin call (margin call takes precedence)
   limit_order_id_type buy_med = create_sell_order(buyer2, asset(33000), bitusd.amount(3000))->id;
   // This buy order above MSSP will be matched with a sell order (limit order with better price takes precedence)
   limit_order_id_type buy_high = create_sell_order(buyer3, asset(111), bitusd.amount(10))->id;

   BOOST_CHECK_EQUAL( 0, get_balance(buyer, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(buyer2, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(buyer3, bitusd) );
   BOOST_CHECK_EQUAL( init_balance - 80, get_balance(buyer, core) );
   BOOST_CHECK_EQUAL( init_balance - 33000, get_balance(buyer2, core) );
   BOOST_CHECK_EQUAL( init_balance - 111, get_balance(buyer3, core) );

   // call and call2's CR is quite high, and debt amount is quite a lot,
   // assume neither of them will be completely filled
   price match_price( bitusd.amount(1) / core.amount(11) );
   share_type call_to_cover = call_id(db).get_max_debt_to_cover(match_price,current_feed.settlement_price,1750);
   share_type call2_to_cover = call2_id(db).get_max_debt_to_cover(match_price,current_feed.settlement_price,1750);
   BOOST_CHECK_LT( call_to_cover.value, call_id(db).debt.value );
   BOOST_CHECK_LT( call2_to_cover.value, call2_id(db).debt.value );
   // even though call2 has a higher CR, since call's TCR is less than call2's TCR,
   // so we expect call will cover less when called
   BOOST_CHECK_LT( call_to_cover.value, call2_to_cover.value );

   // Create a force settlement, will be matched with several call orders
   auto result = force_settle( seller, bitusd.amount(700*4) );
   BOOST_REQUIRE( result.is_type<object_id_type>() );
   force_settlement_id_type settle_id = result.get<object_id_type>();
   BOOST_CHECK( db.find( settle_id ) != nullptr );

   // buy orders won't change
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( buy_low )->for_sale.value, 80 );
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( buy_med )->for_sale.value, 33000 );
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( buy_high )->for_sale.value, 111 );

   // the settle order will match with call, at mssp: 1/11 = 1000/11000
   const call_order_object* tmp_call = db.find<call_order_object>( call_id );
   BOOST_CHECK( tmp_call != nullptr );

   // call will receive call_to_cover, pay 11*call_to_cover
   share_type call_to_pay = call_to_cover * 11;
   share_type call_to_settler = (call_to_cover * 10 * 107 + 99) / 100; // round up, favors settle order
   BOOST_CHECK_EQUAL( 1000 - call_to_cover.value, call.debt.value );
   BOOST_CHECK_EQUAL( 15000 - call_to_pay.value, call.collateral.value );
   // new collateral ratio should be higher than mcr as well as tcr
   BOOST_CHECK( call.debt.value * 10 * 1750 < call.collateral.value * 1000 );
   idump( (call) );
   // borrower's balance doesn't change
   BOOST_CHECK_EQUAL( init_balance - 15000, get_balance(borrower, core) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower, bitusd) );

   // the settle order then will match with call2, at mssp: 1/11 = 1000/11000
   const call_order_object* tmp_call2 = db.find<call_order_object>( call2_id );
   BOOST_CHECK( tmp_call2 != nullptr );

   // call2 will receive call2_to_cover, pay 11*call2_to_cover
   share_type call2_to_pay = call2_to_cover * 11;
   share_type call2_to_settler = (call2_to_cover * 10 * 107 + 99) / 100; // round up, favors settle order
   BOOST_CHECK_EQUAL( 1000 - call2_to_cover.value, call2.debt.value );
   BOOST_CHECK_EQUAL( 15500 - call2_to_pay.value, call2.collateral.value );
   // new collateral ratio should be higher than mcr as well as tcr
   BOOST_CHECK( call2.debt.value * 10 * 2000 < call2.collateral.value * 1000 );
   idump( (call2) );
   // borrower2's balance doesn't change
   BOOST_CHECK_EQUAL( init_balance - 15500, get_balance(borrower2, core) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower2, bitusd) );

   // call3 is not in margin call territory so won't be matched
   BOOST_CHECK_EQUAL( 1000, call3.debt.value );
   BOOST_CHECK_EQUAL( 25000, call3.collateral.value );

   // check the settle order's balance
   BOOST_CHECK_EQUAL( 700 * 4 - call2_to_cover.value - call_to_cover.value,
                      settle_id(db).balance.amount.value );

   // check seller balance
   BOOST_CHECK_EQUAL( 193, get_balance(seller, bitusd) ); // 3000 - 7 - 700*4
   int64_t expected_seller_core_balance = call_to_settler.value + call2_to_settler.value;
   BOOST_CHECK_EQUAL( expected_seller_core_balance, get_balance(seller, core) );

   // asset's force_settled_volume does not change
   BOOST_CHECK_EQUAL( 0, usd_id(db).bitasset_data(db).force_settled_volume.value );

   // generate a block
   generate_block();

} FC_LOG_AND_RETHROW() }

/***
 * BSIP38 "target_collateral_ratio" test after hf core-2481:
 *   matching taker call orders with maker settle orders
 */
BOOST_AUTO_TEST_CASE(tcr_test_hf2481_call_settle)
{ try {

   auto mi = db.get_global_properties().parameters.maintenance_interval;
   generate_blocks(HARDFORK_CORE_2481_TIME - mi);
   generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

   set_expiration( db, trx );

   ACTORS((buyer)(seller)(borrower)(borrower2)(borrower3)(borrower4)(feedproducer));

   const auto& bitusd = create_bitasset("USDBIT", feedproducer_id);
   const auto& core   = asset_id_type()(db);
   asset_id_type usd_id = bitusd.id;

   {
      // set margin call fee ratio
      asset_update_bitasset_operation uop;
      uop.issuer = usd_id(db).issuer;
      uop.asset_to_update = usd_id;
      uop.new_options = usd_id(db).bitasset_data(db).options;
      uop.new_options.extensions.value.margin_call_fee_ratio = 30; // 3%

      trx.clear();
      trx.operations.push_back(uop);
      PUSH_TX(db, trx, ~0);
   }

   int64_t init_balance(1000000);

   transfer(committee_account, buyer_id, asset(init_balance));
   transfer(committee_account, borrower_id, asset(init_balance));
   transfer(committee_account, borrower2_id, asset(init_balance));
   transfer(committee_account, borrower3_id, asset(init_balance));
   transfer(committee_account, borrower4_id, asset(init_balance));
   update_feed_producers( bitusd, {feedproducer.id} );

   price_feed current_feed;
   current_feed.maintenance_collateral_ratio = 1750;
   current_feed.maximum_short_squeeze_ratio = 1100;
   current_feed.settlement_price = bitusd.amount( 1 ) / core.amount(5);
   publish_feed( bitusd, feedproducer, current_feed );
   // start out with 300% collateral, call price is 15/1.75 CORE/USD = 60/7, tcr 170% is lower than 175%
   const call_order_object& call = *borrow( borrower, bitusd.amount(1000), asset(15000), 1700);
   call_order_id_type call_id = call.id;
   // create another position with 310% collateral, call price is 15.5/1.75 CORE/USD = 62/7, tcr 200% > 175%
   const call_order_object& call2 = *borrow( borrower2, bitusd.amount(1000), asset(15500), 2000);
   call_order_id_type call2_id = call2.id;
   // create yet another position with 500% collateral, call price is 25/1.75 CORE/USD = 100/7, no tcr
   const call_order_object& call3 = *borrow( borrower3, bitusd.amount(1000), asset(25000));
   // create a small position with 320% collateral, call price is 16/1.75 CORE/USD = 64/7, no tcr
   const call_order_object& call4 = *borrow( borrower4, bitusd.amount(10), asset(160) );
   call_order_id_type call4_id = call4.id;

   transfer(borrower, seller, bitusd.amount(1000));
   transfer(borrower2, seller, bitusd.amount(1000));
   transfer(borrower3, seller, bitusd.amount(1000));

   BOOST_CHECK_EQUAL( 1000, call.debt.value );
   BOOST_CHECK_EQUAL( 15000, call.collateral.value );
   BOOST_CHECK_EQUAL( 1000, call2.debt.value );
   BOOST_CHECK_EQUAL( 15500, call2.collateral.value );
   BOOST_CHECK_EQUAL( 1000, call3.debt.value );
   BOOST_CHECK_EQUAL( 25000, call3.collateral.value );
   BOOST_CHECK_EQUAL( 3000, get_balance(seller, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(seller, core) );
   BOOST_CHECK_EQUAL( 3000, get_balance(seller, bitusd) );
   BOOST_CHECK_EQUAL( init_balance - 15000, get_balance(borrower, core) );
   BOOST_CHECK_EQUAL( init_balance - 15500, get_balance(borrower2, core) );
   BOOST_CHECK_EQUAL( init_balance - 25000, get_balance(borrower3, core) );
   BOOST_CHECK_EQUAL( init_balance - 160, get_balance(borrower4, core) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower2, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower3, bitusd) );
   BOOST_CHECK_EQUAL( 10, get_balance(borrower4, bitusd) );

   // This sell order above MSSP will not be matched with a call
   limit_order_id_type sell_high = create_sell_order(seller, bitusd.amount(7), core.amount(78))->id;
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( sell_high )->for_sale.value, 7 );

   BOOST_CHECK_EQUAL( 2993, get_balance(seller, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(seller, core) );

   // This buy order is too low will not be matched with a sell order
   limit_order_id_type buy_low = create_sell_order(buyer, asset(80), bitusd.amount(10))->id;

   BOOST_CHECK_EQUAL( 0, get_balance(buyer, bitusd) );
   BOOST_CHECK_EQUAL( init_balance - 80, get_balance(buyer, core) );

   // Create a sell order which will be matched with several call orders later, price 1/9
   limit_order_id_type sell_id = create_sell_order(seller, bitusd.amount(500), core.amount(4500) )->id;
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( sell_id )->for_sale.value, 500 );

   // Create a force settlement, will be matched with several call orders later
   auto result = force_settle( seller, bitusd.amount(2400) );
   BOOST_REQUIRE( result.is_type<object_id_type>() );
   force_settlement_id_type settle_id = result.get<object_id_type>();
   BOOST_CHECK( db.find( settle_id ) != nullptr );

   // prepare price feed to get call and call2 (but not call3) into margin call territory
   current_feed.settlement_price = bitusd.amount( 1 ) / core.amount(10);

   // call and call2's CR is quite high, and debt amount is quite a lot,
   // assume neither of them will be completely filled
   price match_price = sell_id(db).sell_price * ratio_type(107,110);
   share_type call_to_cover = call_id(db).get_max_debt_to_cover(match_price,current_feed.settlement_price,1750);
   share_type call2_to_cover = call2_id(db).get_max_debt_to_cover(match_price,current_feed.settlement_price,1750);
   BOOST_CHECK_LT( call_to_cover.value, call_id(db).debt.value );
   BOOST_CHECK_LT( call2_to_cover.value, call2_id(db).debt.value );
   // even though call2 has a higher CR, since call's TCR is less than call2's TCR,
   // so we expect call will cover less when called
   BOOST_CHECK_LT( call_to_cover.value, call2_to_cover.value );

   call_order_object call2_copy = call2;

   // adjust price feed to get call and call2 (but not call3) into margin call territory
   publish_feed( bitusd, feedproducer, current_feed );
   // settlement price = 1/10, mssp = 1/11, mcop = 10/107, mcpr = 110/107

   // firstly the limit order will match with call, at limit order's price: 1/9
   const call_order_object* tmp_call = db.find<call_order_object>( call_id );
   BOOST_CHECK( tmp_call != nullptr );

   // call will receive call_to_cover, pay 9*call_to_cover
   share_type call_to_pay = (call_to_cover * 9 * 110 + 106) / 107; // round up since it's smaller
   BOOST_CHECK_EQUAL( 1000 - call_to_cover.value, call.debt.value );
   BOOST_CHECK_EQUAL( 15000 - call_to_pay.value, call.collateral.value );
   // new collateral ratio should be higher than mcr as well as tcr
   BOOST_CHECK( call.debt.value * 10 * 1750 < call.collateral.value * 1000 );
   idump( (call_to_pay)(call_to_cover)(call) );
   // borrower's balance doesn't change
   BOOST_CHECK_EQUAL( init_balance - 15000, get_balance(borrower, core) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower, bitusd) );

   // the limit order then will match with call2, at limit order's price: 1/9
   const call_order_object* tmp_call2 = db.find<call_order_object>( call2_id );
   BOOST_CHECK( tmp_call2 != nullptr );

   // if the limit is big enough, call2 will receive call2_to_cover, pay 9*call2_to_cover
   // however it's not the case, so call2 will receive less
   call2_to_cover = 500 - call_to_cover;
   share_type call2_to_pay = call2_to_cover * 9 * 110 / 107; // round down since it's larger

   // borrower2's balance doesn't change
   BOOST_CHECK_EQUAL( init_balance - 15500, get_balance(borrower2, core) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower2, bitusd) );

   // call4 will match with the settle order, since it has no tcr, it will be fully closed
   // match price is 1/11
   const call_order_object* tmp_call4 = db.find<call_order_object>( call4_id );
   BOOST_CHECK( tmp_call4 == nullptr );

   // borrower4 balance changes
   BOOST_CHECK_EQUAL( init_balance - 110, get_balance(borrower4, core) );
   BOOST_CHECK_EQUAL( 10, get_balance(borrower4, bitusd) );

   // call2 is still in margin call territory after matched with limit order, now it matches with settle order
   price call_pays_price( asset(1, usd_id), asset(11) );
   call2_copy.debt -= call2_to_cover;
   call2_copy.collateral -= call2_to_pay;
   auto call2_to_cover2 = call2_copy.get_max_debt_to_cover(call_pays_price,current_feed.settlement_price,1750);
   BOOST_CHECK_GT( call2_to_cover2.value, 0 );
   share_type call2_to_pay2 = call2_to_cover2 * 11;
   BOOST_CHECK_EQUAL( 1000 - call2_to_cover.value - call2_to_cover2.value, call2.debt.value );
   BOOST_CHECK_EQUAL( 15500 - call2_to_pay.value - call2_to_pay2.value, call2.collateral.value );
   idump( (call2_to_pay)(call2_to_cover)(call2_to_pay2)(call2_to_cover2)(call2) );

   // call3 is not in margin call territory so won't be matched
   BOOST_CHECK_EQUAL( 1000, call3.debt.value );
   BOOST_CHECK_EQUAL( 25000, call3.collateral.value );

   // sell_id is completely filled
   BOOST_CHECK( !db.find<limit_order_object>( sell_id ) );

   // settle order is not fully filled
   BOOST_CHECK( db.find( settle_id ) != nullptr );

   // check seller balance
   BOOST_CHECK_EQUAL( 93, get_balance(seller, bitusd) ); // 3000 - 7 - 500 - 2400
   BOOST_CHECK_EQUAL( 4500 + 107 + (call2_to_cover2.value * 107 + 9) / 10, // round up
                      get_balance(seller, core) ); // 500*9 + 10*10.7 + call2_cover2 * 10.7

   // buy_low's price is too low that won't be matched
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( buy_low )->for_sale.value, 80 );

   // Can not reduce CR of a call order to trigger a margin call but not get fully filled
   BOOST_CHECK_THROW( borrow( borrower_id(db), asset(10, usd_id), asset(0), 1700), fc::exception );

   // Can not create a new call order that is partially called instantly
   BOOST_CHECK_THROW( borrow( borrower4_id(db), asset(10, usd_id), asset(160), 1700), fc::exception );

   // generate a block
   generate_block();

} FC_LOG_AND_RETHROW() }

/***
 * Request force settlement before hard fork, match taker call orders with maker settle orders at hard fork time
 */
BOOST_AUTO_TEST_CASE(hf2481_cross_test)
{ try {

   auto mi = db.get_global_properties().parameters.maintenance_interval;
   generate_blocks(HARDFORK_CORE_2481_TIME - mi);

   set_expiration( db, trx );

   ACTORS((buyer)(seller)(borrower)(borrower2)(borrower3)(borrower4)(feedproducer));

   const auto& bitusd = create_bitasset("USDBIT", feedproducer_id);
   const auto& core   = asset_id_type()(db);
   asset_id_type usd_id = bitusd.id;

   {
      // set margin call fee ratio
      asset_update_bitasset_operation uop;
      uop.issuer = usd_id(db).issuer;
      uop.asset_to_update = usd_id;
      uop.new_options = usd_id(db).bitasset_data(db).options;
      uop.new_options.feed_lifetime_sec = mi * 10;
      uop.new_options.force_settlement_delay_sec = mi * 10;
      uop.new_options.extensions.value.margin_call_fee_ratio = 30; // 3%

      trx.clear();
      trx.operations.push_back(uop);
      PUSH_TX(db, trx, ~0);
   }

   int64_t init_balance(1000000);

   transfer(committee_account, buyer_id, asset(init_balance));
   transfer(committee_account, borrower_id, asset(init_balance));
   transfer(committee_account, borrower2_id, asset(init_balance));
   transfer(committee_account, borrower3_id, asset(init_balance));
   transfer(committee_account, borrower4_id, asset(init_balance));
   update_feed_producers( bitusd, {feedproducer.id} );

   price_feed current_feed;
   current_feed.maintenance_collateral_ratio = 1750;
   current_feed.maximum_short_squeeze_ratio = 1100;
   current_feed.settlement_price = bitusd.amount( 1 ) / core.amount(5);
   publish_feed( bitusd, feedproducer, current_feed );
   // start out with 300% collateral, call price is 15/1.75 CORE/USD = 60/7, tcr 170% is lower than 175%
   const call_order_object& call = *borrow( borrower, bitusd.amount(1000), asset(15000), 1700);
   call_order_id_type call_id = call.id;
   // create another position with 310% collateral, call price is 15.5/1.75 CORE/USD = 62/7, tcr 200% > 175%
   const call_order_object& call2 = *borrow( borrower2, bitusd.amount(1000), asset(15500), 2000);
   call_order_id_type call2_id = call2.id;
   // create yet another position with 500% collateral, call price is 25/1.75 CORE/USD = 100/7, no tcr
   const call_order_object& call3 = *borrow( borrower3, bitusd.amount(1000), asset(25000));
   call_order_id_type call3_id = call3.id;
   // create a small position with 320% collateral, call price is 16/1.75 CORE/USD = 64/7, no tcr
   const call_order_object& call4 = *borrow( borrower4, bitusd.amount(10), asset(160) );
   call_order_id_type call4_id = call4.id;

   transfer(borrower, seller, bitusd.amount(1000));
   transfer(borrower2, seller, bitusd.amount(1000));
   transfer(borrower3, seller, bitusd.amount(1000));

   BOOST_CHECK_EQUAL( 1000, call.debt.value );
   BOOST_CHECK_EQUAL( 15000, call.collateral.value );
   BOOST_CHECK_EQUAL( 1000, call2.debt.value );
   BOOST_CHECK_EQUAL( 15500, call2.collateral.value );
   BOOST_CHECK_EQUAL( 1000, call3.debt.value );
   BOOST_CHECK_EQUAL( 25000, call3.collateral.value );
   BOOST_CHECK_EQUAL( 3000, get_balance(seller, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(seller, core) );
   BOOST_CHECK_EQUAL( 3000, get_balance(seller, bitusd) );
   BOOST_CHECK_EQUAL( init_balance - 15000, get_balance(borrower, core) );
   BOOST_CHECK_EQUAL( init_balance - 15500, get_balance(borrower2, core) );
   BOOST_CHECK_EQUAL( init_balance - 25000, get_balance(borrower3, core) );
   BOOST_CHECK_EQUAL( init_balance - 160, get_balance(borrower4, core) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower2, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower3, bitusd) );
   BOOST_CHECK_EQUAL( 10, get_balance(borrower4, bitusd) );

   // This sell order above MSSP will not be matched with a call
   limit_order_id_type sell_high = create_sell_order(seller, bitusd.amount(7), core.amount(78))->id;
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( sell_high )->for_sale.value, 7 );

   BOOST_CHECK_EQUAL( 2993, get_balance(seller, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(seller, core) );

   // This buy order is too low will not be matched with a sell order
   limit_order_id_type buy_low = create_sell_order(buyer, asset(80), bitusd.amount(10))->id;

   BOOST_CHECK_EQUAL( 0, get_balance(buyer, bitusd) );
   BOOST_CHECK_EQUAL( init_balance - 80, get_balance(buyer, core) );

   // Create a sell order which will be matched with several call orders later, price 1/9
   limit_order_id_type sell_id = create_sell_order(seller, bitusd.amount(500), core.amount(4500) )->id;
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( sell_id )->for_sale.value, 500 );

   // Create a force settlement, will be matched with several call orders later
   auto result = force_settle( seller, bitusd.amount(2400) );
   BOOST_REQUIRE( result.is_type<object_id_type>() );
   force_settlement_id_type settle_id = result.get<object_id_type>();
   BOOST_CHECK( db.find( settle_id ) != nullptr );

   BOOST_CHECK_EQUAL( 2400, settle_id(db).balance.amount.value );

   // prepare price feed to get call and call2 (but not call3) into margin call territory
   current_feed.settlement_price = bitusd.amount( 1 ) / core.amount(10);

   // call and call2's CR is quite high, and debt amount is quite a lot,
   // assume neither of them will be completely filled
   price match_price = sell_id(db).sell_price * ratio_type(107,110);
   share_type call_to_cover = call_id(db).get_max_debt_to_cover(match_price,current_feed.settlement_price,1750);
   share_type call2_to_cover = call2_id(db).get_max_debt_to_cover(match_price,current_feed.settlement_price,1750);
   BOOST_CHECK_LT( call_to_cover.value, call_id(db).debt.value );
   BOOST_CHECK_LT( call2_to_cover.value, call2_id(db).debt.value );
   // even though call2 has a higher CR, since call's TCR is less than call2's TCR,
   // so we expect call will cover less when called
   BOOST_CHECK_LT( call_to_cover.value, call2_to_cover.value );

   call_order_object call2_copy = call2;

   // adjust price feed to get call and call2 (but not call3) into margin call territory
   publish_feed( bitusd, feedproducer, current_feed );
   // settlement price = 1/10, mssp = 1/11, mcop = 10/107, mcpr = 110/107

   generate_block();

   // firstly the limit order will match with call, at limit order's price: 1/9
   const call_order_object* tmp_call = db.find<call_order_object>( call_id );
   BOOST_CHECK( tmp_call != nullptr );

   // call will receive call_to_cover, pay 9*call_to_cover
   share_type call_to_pay = (call_to_cover * 9 * 110 + 106) / 107; // round up since it's smaller
   BOOST_CHECK_EQUAL( 1000 - call_to_cover.value, call_id(db).debt.value );
   BOOST_CHECK_EQUAL( 15000 - call_to_pay.value, call_id(db).collateral.value );
   // new collateral ratio should be higher than mcr as well as tcr
   BOOST_CHECK( call_id(db).debt.value * 10 * 1750 < call_id(db).collateral.value * 1000 );
   idump( (call_to_pay)(call_to_cover)(call_id(db)) );
   // borrower's balance doesn't change
   BOOST_CHECK_EQUAL( init_balance - 15000, get_balance(borrower_id, asset_id_type()) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower_id, usd_id) );

   // the limit order then will match with call2, at limit order's price: 1/9
   const call_order_object* tmp_call2 = db.find<call_order_object>( call2_id );
   BOOST_CHECK( tmp_call2 != nullptr );

   // if the limit is big enough, call2 will receive call2_to_cover, pay 9*call2_to_cover
   // however it's not the case, so call2 will receive less
   call2_to_cover = 500 - call_to_cover;
   share_type call2_to_pay = call2_to_cover * 9 * 110 / 107; // round down since it's larger

   // borrower2's balance doesn't change
   BOOST_CHECK_EQUAL( init_balance - 15500, get_balance(borrower2_id, asset_id_type()) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower2_id, usd_id) );

   // sell_id is completely filled
   BOOST_CHECK( !db.find<limit_order_object>( sell_id ) );

   // all call orders are still there
   BOOST_CHECK( db.find( call_id ) != nullptr );
   BOOST_CHECK( db.find( call2_id ) != nullptr );
   BOOST_CHECK( db.find( call3_id ) != nullptr );
   BOOST_CHECK( db.find( call4_id ) != nullptr );

   BOOST_CHECK_EQUAL( 1000 - call2_to_cover.value, call2_id(db).debt.value );
   BOOST_CHECK_EQUAL( 15500 - call2_to_pay.value, call2_id(db).collateral.value );

   idump( (call2_to_pay)(call2_to_cover)(call2_id(db)) );

   // settle order does not change
   BOOST_CHECK( db.find( settle_id ) != nullptr );
   BOOST_CHECK_EQUAL( 2400, settle_id(db).balance.amount.value );

   // check borrower4's balances
   BOOST_CHECK_EQUAL( init_balance - 160, get_balance(borrower4_id, asset_id_type()) );
   BOOST_CHECK_EQUAL( 10, get_balance(borrower4_id, usd_id) );

   // check seller balance
   BOOST_CHECK_EQUAL( 93, get_balance(seller_id, usd_id) ); // 3000 - 7 - 500 - 2400
   BOOST_CHECK_EQUAL( 4500, get_balance(seller_id, asset_id_type()) ); // 500*9

   // call3 is not in margin call territory so won't be matched
   BOOST_CHECK_EQUAL( 1000, call3_id(db).debt.value );
   BOOST_CHECK_EQUAL( 25000, call3_id(db).collateral.value );

   // buy_low's price is too low that won't be matched
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( buy_low )->for_sale.value, 80 );

   // pass the hard fork time
   generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

   // call4 will match with the settle order, since it has no tcr, it will be fully closed
   // match price is 1/11
   const call_order_object* tmp_call4 = db.find<call_order_object>( call4_id );
   BOOST_CHECK( tmp_call4 == nullptr );

   // borrower4 balance changes
   BOOST_CHECK_EQUAL( init_balance - 110, get_balance(borrower4_id, asset_id_type()) );
   BOOST_CHECK_EQUAL( 10, get_balance(borrower4_id, usd_id) );

   // call2 is still in margin call territory after matched with limit order, now it matches with settle order
   price call_pays_price( asset(1, usd_id), asset(11) );
   call2_copy.debt -= call2_to_cover;
   call2_copy.collateral -= call2_to_pay;
   auto call2_to_cover2 = call2_copy.get_max_debt_to_cover(call_pays_price,current_feed.settlement_price,1750);
   BOOST_CHECK_GT( call2_to_cover2.value, 0 );
   share_type call2_to_pay2 = call2_to_cover2 * 11;
   BOOST_CHECK_EQUAL( 1000 - call2_to_cover.value - call2_to_cover2.value, call2_id(db).debt.value );
   BOOST_CHECK_EQUAL( 15500 - call2_to_pay.value - call2_to_pay2.value, call2_id(db).collateral.value );
   idump( (call2_to_pay)(call2_to_cover)(call2_to_pay2)(call2_to_cover2)(call2_id(db)) );

   // call3 is not in margin call territory so won't be matched
   BOOST_CHECK_EQUAL( 1000, call3_id(db).debt.value );
   BOOST_CHECK_EQUAL( 25000, call3_id(db).collateral.value );

   // settle order is not fully filled
   BOOST_CHECK( db.find( settle_id ) != nullptr );
   BOOST_CHECK_EQUAL( 2400 - 10 - call2_to_cover2.value, settle_id(db).balance.amount.value ); // call4, call2

   // check seller balance
   BOOST_CHECK_EQUAL( 93, get_balance(seller_id, usd_id) ); // 3000 - 7 - 500 - 2400
   BOOST_CHECK_EQUAL( 4500 + 107 + (call2_to_cover2.value * 107 + 9) / 10, // round up
                      get_balance(seller_id, asset_id_type()) ); // 500*9 + 10*10.7 + call2_cover2 * 10.7

   // buy_low's price is too low that won't be matched
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( buy_low )->for_sale.value, 80 );

   // generate a block
   generate_block();

} FC_LOG_AND_RETHROW() }

/***
 * Matching taker call orders with maker settle orders and triggers blackswan event
 */
BOOST_AUTO_TEST_CASE(call_settle_blackswan)
{ try {

   auto mi = db.get_global_properties().parameters.maintenance_interval;
   generate_blocks(HARDFORK_CORE_2481_TIME - mi);
   generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

   set_expiration( db, trx );

   ACTORS((buyer)(seller)(borrower)(borrower2)(borrower3)(borrower4)(borrower5)(feedproducer));

   const auto& bitusd = create_bitasset("USDBIT", feedproducer_id);
   const auto& core   = asset_id_type()(db);
   asset_id_type usd_id = bitusd.id;

   {
      // set margin call fee ratio
      asset_update_bitasset_operation uop;
      uop.issuer = usd_id(db).issuer;
      uop.asset_to_update = usd_id;
      uop.new_options = usd_id(db).bitasset_data(db).options;
      uop.new_options.extensions.value.margin_call_fee_ratio = 30; // 3%

      trx.clear();
      trx.operations.push_back(uop);
      PUSH_TX(db, trx, ~0);
   }

   int64_t init_balance(1000000);

   transfer(committee_account, buyer_id, asset(init_balance));
   transfer(committee_account, borrower_id, asset(init_balance));
   transfer(committee_account, borrower2_id, asset(init_balance));
   transfer(committee_account, borrower3_id, asset(init_balance));
   transfer(committee_account, borrower4_id, asset(init_balance));
   transfer(committee_account, borrower5_id, asset(init_balance));
   update_feed_producers( bitusd, {feedproducer.id} );

   price_feed current_feed;
   current_feed.maintenance_collateral_ratio = 1750;
   current_feed.maximum_short_squeeze_ratio = 1100;
   current_feed.settlement_price = bitusd.amount( 1 ) / core.amount(5);
   publish_feed( bitusd, feedproducer, current_feed );
   // start out with 300% collateral, call price is 15/1.75 CORE/USD = 60/7, tcr 170% is lower than 175%
   const call_order_object& call = *borrow( borrower, bitusd.amount(1000), asset(15000), 1700);
   call_order_id_type call_id = call.id;
   // create another position with 310% collateral, call price is 15.5/1.75 CORE/USD = 62/7, tcr 200% > 175%
   const call_order_object& call2 = *borrow( borrower2, bitusd.amount(1000), asset(15500), 2000);
   call_order_id_type call2_id = call2.id;
   // create yet another position with 500% collateral, call price is 25/1.75 CORE/USD = 100/7, no tcr
   const call_order_object& call3 = *borrow( borrower3, bitusd.amount(1000), asset(25000));
   call_order_id_type call3_id = call3.id;
   // create a small position with 320% collateral, call price is 16/1.75 CORE/USD = 64/7, no tcr
   const call_order_object& call4 = *borrow( borrower4, bitusd.amount(10), asset(160) );
   call_order_id_type call4_id = call4.id;
   // create yet another position with 900% collateral, call price is 45/1.75 CORE/USD = 180/7, no tcr
   const call_order_object& call5 = *borrow( borrower5, bitusd.amount(1000), asset(45000));
   call_order_id_type call5_id = call5.id;

   transfer(borrower, seller, bitusd.amount(1000));
   transfer(borrower2, seller, bitusd.amount(1000));
   transfer(borrower3, seller, bitusd.amount(1000));

   BOOST_CHECK_EQUAL( 1000, call.debt.value );
   BOOST_CHECK_EQUAL( 15000, call.collateral.value );
   BOOST_CHECK_EQUAL( 1000, call2.debt.value );
   BOOST_CHECK_EQUAL( 15500, call2.collateral.value );
   BOOST_CHECK_EQUAL( 1000, call3.debt.value );
   BOOST_CHECK_EQUAL( 25000, call3.collateral.value );
   BOOST_CHECK_EQUAL( 3000, get_balance(seller, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(seller, core) );
   BOOST_CHECK_EQUAL( 3000, get_balance(seller, bitusd) );
   BOOST_CHECK_EQUAL( init_balance - 15000, get_balance(borrower, core) );
   BOOST_CHECK_EQUAL( init_balance - 15500, get_balance(borrower2, core) );
   BOOST_CHECK_EQUAL( init_balance - 25000, get_balance(borrower3, core) );
   BOOST_CHECK_EQUAL( init_balance - 160, get_balance(borrower4, core) );
   BOOST_CHECK_EQUAL( init_balance - 45000, get_balance(borrower5, core) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower2, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower3, bitusd) );
   BOOST_CHECK_EQUAL( 10, get_balance(borrower4, bitusd) );
   BOOST_CHECK_EQUAL( 1000, get_balance(borrower5, bitusd) );

   // This sell order above MCOP will not be matched with a call
   limit_order_id_type sell_high = create_sell_order(seller, bitusd.amount(7), core.amount(150))->id;
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( sell_high )->for_sale.value, 7 );

   BOOST_CHECK_EQUAL( 2993, get_balance(seller, bitusd) );
   BOOST_CHECK_EQUAL( 0, get_balance(seller, core) );

   // This buy order is too low will not be matched with a sell order
   limit_order_id_type buy_low = create_sell_order(buyer, asset(80), bitusd.amount(10))->id;

   BOOST_CHECK_EQUAL( 0, get_balance(buyer, bitusd) );
   BOOST_CHECK_EQUAL( init_balance - 80, get_balance(buyer, core) );

   // Create a sell order which will be matched with several call orders later, price 1/9
   limit_order_id_type sell_id = create_sell_order(seller, bitusd.amount(1000), core.amount(9000) )->id;
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( sell_id )->for_sale.value, 1000 );

   // Create a force settlement, will be matched with several call orders later
   auto result = force_settle( seller, bitusd.amount(400) );
   BOOST_REQUIRE( result.is_type<object_id_type>() );
   force_settlement_id_type settle_id = result.get<object_id_type>();
   BOOST_CHECK( db.find( settle_id ) != nullptr );

   // Create another force settlement
   result = force_settle( seller, bitusd.amount(100) );
   BOOST_REQUIRE( result.is_type<object_id_type>() );
   force_settlement_id_type settle2_id = result.get<object_id_type>();
   BOOST_CHECK( db.find( settle2_id ) != nullptr );

   // prepare price feed to get call, call2, call3 and call4 (but not call5) into margin call territory
   current_feed.settlement_price = bitusd.amount( 1 ) / core.amount(20);

   // since the sell limit order's price is low, and TCR is set for both call and call2,
   // call and call2 will match with the sell limit order
   price match_price = sell_id(db).sell_price * ratio_type(107,110);
   share_type call_to_cover = call_id(db).get_max_debt_to_cover(match_price,current_feed.settlement_price,1750);
   share_type call2_to_cover = call2_id(db).get_max_debt_to_cover(match_price,current_feed.settlement_price,1750);
   BOOST_CHECK_LT( call_to_cover.value, call_id(db).debt.value );
   BOOST_CHECK_LT( call2_to_cover.value, call2_id(db).debt.value );
   // even though call2 has a higher CR, since call's TCR is less than call2's TCR,
   // so we expect call will cover less when called
   BOOST_CHECK_LT( call_to_cover.value, call2_to_cover.value );

   call_order_object call_copy = call;
   call_order_object call2_copy = call2;
   call_order_object call3_copy = call3;
   call_order_object call4_copy = call4;
   call_order_object call5_copy = call5;

   // adjust price feed to get call, call2, call3 and call4 (but not call5) into margin call territory
   publish_feed( bitusd, feedproducer, current_feed );
   // settlement price = 1/20, mssp = 1/22, mcop = 20/107, mcpr = 110/107

   share_type expected_margin_call_fees = 0;

   // firstly the sell limit order will match with call, at limit order's price: 1/9
   // call will receive call_to_cover, pay 9*call_to_cover
   share_type call_to_pay = (call_to_cover * 9 * 110 + 106) / 107; // round up since it's smaller
   call_copy.debt -= call_to_cover;
   call_copy.collateral -= call_to_pay;

   // the limit order then will match with call2, at limit order's price: 1/9
   // if the limit is big enough, call2 will receive call2_to_cover, pay 9*call2_to_cover
   // however it's not the case, so call2 will receive less
   call2_to_cover = 1000 - call_to_cover;
   share_type call2_to_pay = call2_to_cover * 9 * 110 / 107; // round down since it's larger
   call2_copy.debt -= call2_to_cover;
   call2_copy.collateral -= call2_to_pay;

   // sell_id is completely filled
   BOOST_CHECK( !db.find<limit_order_object>( sell_id ) );
   share_type margin_call_fee_limit = call_to_pay + call2_to_pay - 9000;
   expected_margin_call_fees += margin_call_fee_limit;

   // now call4 has the lowest CR
   // call4 will match with the settle order, since it is small and has too few collateral, it will be fully closed
   // and it will lose all collateral, 160
   // call_pays_price is 1/16, settle_receives_price is (1/16)*(110/107) = 55/856
   share_type settle_receives4 = 157; // (10 * 856 + 54) / 55; // round up
   share_type margin_call_fee_settle_4 = 3; // 160 - 157
   expected_margin_call_fees += margin_call_fee_settle_4;
   // borrower4 balance does not change
   BOOST_CHECK_EQUAL( init_balance - 160, get_balance(borrower4, core) );
   BOOST_CHECK_EQUAL( 10, get_balance(borrower4, bitusd) );

   // now call2 has the lowest CR
   // call2 is still in margin call territory after matched with limit order, now it matches with settle orders
   // the settle orders are too small to fill call2
   share_type call2_to_cover1 = 390; // 400 - 10
   share_type call2_to_pay1 = call2_to_cover1 * call2_copy.collateral / call2_copy.debt; // round down

   share_type settle_receives2 = call2_to_pay1 * 107 / 110; // round down
   share_type margin_call_fee_settle_2 = call2_to_pay1 - settle_receives2;
   expected_margin_call_fees += margin_call_fee_settle_2;

   call2_copy.debt -= call2_to_cover1;
   call2_copy.collateral -= call2_to_pay1;

   // call2 matches with the other settle order
   share_type call2_to_cover2 = 100; // 400 - 10
   share_type call2_to_pay2 = call2_to_cover2 * call2_copy.collateral / call2_copy.debt; // round down

   share_type settle2_receives2 = call2_to_pay2 * 107 / 110; // round down
   share_type margin_call_fee_settle2_2 = call2_to_pay2 - settle2_receives2;
   expected_margin_call_fees += margin_call_fee_settle2_2;

   call2_copy.debt -= call2_to_cover2;
   call2_copy.collateral -= call2_to_pay2;

   // settle order is fully filled
   BOOST_CHECK( db.find( settle_id ) == nullptr );

   // blackswan event occurs
   BOOST_CHECK( usd_id(db).bitasset_data(db).has_settlement() );
   BOOST_CHECK( db.find( call_id ) == nullptr );
   BOOST_CHECK( db.find( call2_id ) == nullptr );
   BOOST_CHECK( db.find( call3_id ) == nullptr );
   BOOST_CHECK( db.find( call4_id ) == nullptr );
   BOOST_CHECK( db.find( call5_id ) == nullptr );

   share_type expected_gs_fund = 0;

   idump( (call2_copy) );

   // call2 has the lowest CR below required
   share_type call2_to_gs_fund = (call2_copy.collateral * 10 + 10) / 11; // round up
   share_type margin_call_fee_gs_2 = call2_copy.collateral - call2_to_gs_fund;
   expected_margin_call_fees += margin_call_fee_gs_2;
   expected_gs_fund += call2_to_gs_fund;
   // GS price (margin calls to pay) = call2_copy.collateral / call2_copy.debt
   // GS price (all positions to fund) = (call2_copy.collateral * 10) / (call2_copy.debt * 11)

   // borrower2 balance does not change
   BOOST_CHECK_EQUAL( init_balance - 15500, get_balance(borrower2, core) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower2, bitusd) );

   // call3 is in margin call territory
   share_type call3_to_pay_gs = ( call3_copy.debt * call2_copy.collateral + call2_copy.debt - 1 ) / call2_copy.debt;
   share_type call3_to_gs_fund = ( call3_copy.debt * call2_copy.collateral * 10 + call2_copy.debt * 11 - 1 )
                                 / (call2_copy.debt * 11);
   share_type margin_call_fee_gs_3 = call3_to_pay_gs - call3_to_gs_fund;
   expected_margin_call_fees += margin_call_fee_gs_3;
   expected_gs_fund += call3_to_gs_fund;

   // borrower3 balance changes -- some collateral returned
   BOOST_CHECK_EQUAL( init_balance - call3_to_pay_gs.value, get_balance(borrower3, core) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower3, bitusd) );

   // call is not in margin call territory
   share_type call_to_gs_fund = ( call_copy.debt * call2_copy.collateral * 10 + call2_copy.debt * 11 - 1 )
                                 / (call2_copy.debt * 11);
   share_type call_to_pay_gs = call_to_gs_fund;
   expected_gs_fund += call_to_gs_fund;
   // no fee

   // borrower balance changes -- some collateral returned
   BOOST_CHECK_EQUAL( init_balance - call_to_pay.value - call_to_pay_gs.value, get_balance(borrower, core) );
   BOOST_CHECK_EQUAL( 0, get_balance(borrower, bitusd) );

   // call5 is not in margin call territory
   share_type call5_to_gs_fund = ( call5_copy.debt * call2_copy.collateral * 10 + call2_copy.debt * 11 - 1 )
                                 / (call2_copy.debt * 11);
   share_type call5_to_pay_gs = call5_to_gs_fund;
   expected_gs_fund += call5_to_gs_fund;
   // no fee

   // borrower5 balance changes -- some collateral returned
   BOOST_CHECK_EQUAL( init_balance - call5_to_pay_gs.value, get_balance(borrower5, core) );
   BOOST_CHECK_EQUAL( 1000, get_balance(borrower5, bitusd) );

   // check seller balance
   BOOST_CHECK_EQUAL( 1493, get_balance(seller, bitusd) ); // 3000 - 7 - 1000 - 400 - 100
   BOOST_CHECK_EQUAL( 9000 + settle_receives4.value + settle_receives2.value + settle2_receives2.value,
                      get_balance(seller, core) ); // 1000*9 + 160*107/110 + 490 * call2_cr * 107/110

   // buy_low's price is too low that won't be matched
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( buy_low )->for_sale.value, 80 );

   // sell_high is not matched
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( sell_high )->for_sale.value, 7 );

   // check gs fund
   BOOST_CHECK_EQUAL( usd_id(db).bitasset_data(db).settlement_fund.value, expected_gs_fund.value );
   // force_settled_volume is 0
   BOOST_CHECK_EQUAL( usd_id(db).bitasset_data(db).force_settled_volume.value, 0 );

   // check margin call fees
   BOOST_CHECK_EQUAL( usd_id(db).dynamic_asset_data_id(db).accumulated_collateral_fees.value,
                      expected_margin_call_fees.value );

   // generate a block
   BOOST_TEST_MESSAGE( "Generate a block" );
   generate_block();
   BOOST_TEST_MESSAGE( "Check again" );

   // buy_low's price is too low that won't be matched
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( buy_low )->for_sale.value, 80 );

   // sell_high is not matched
   BOOST_CHECK_EQUAL( db.find<limit_order_object>( sell_high )->for_sale.value, 7 );

   // check gs fund
   BOOST_CHECK_EQUAL( usd_id(db).bitasset_data(db).settlement_fund.value, expected_gs_fund.value );
   // force_settled_volume is 0
   BOOST_CHECK_EQUAL( usd_id(db).bitasset_data(db).force_settled_volume.value, 0 );

   // check margin call fees
   BOOST_CHECK_EQUAL( usd_id(db).dynamic_asset_data_id(db).accumulated_collateral_fees.value,
                      expected_margin_call_fees.value );

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
