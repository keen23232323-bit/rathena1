// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "int_market.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>

#include <common/malloc.hpp>
#include <common/mmo.hpp>
#include <common/showmsg.hpp>
#include <common/socket.hpp>
#include <common/sql.hpp>
#include <common/strlib.hpp>
#include <common/timer.hpp>
#include <common/utilities.hpp>

#include "char.hpp"
#include "char_mapif.hpp"
#include "inter.hpp"
#include "int_mail.hpp"

using namespace rathena;

// market_id -> market_data
static std::unordered_map<uint32, std::shared_ptr<struct market_data>> market_db;

TIMER_FUNC(market_end_timer);

void market_delete(std::shared_ptr<struct market_data> market) {
	uint32 market_id = market->market_id;
	if (SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `custom_market` WHERE `market_id` = '%d'", market_id))
		Sql_ShowDebug(sql_handle);

	if (market->market_end_timer != INVALID_TIMER)
		delete_timer(market->market_end_timer, market_end_timer);

	market_db.erase(market_id);
}

void market_save(std::shared_ptr<struct market_data> market) {
	StringBuf buf;
	SqlStmt stmt{ *sql_handle };

	StringBuf_Init(&buf);
	StringBuf_Printf(&buf, "UPDATE `custom_market` SET `buyer_id` = '%d', `buyer_name` = ?, `price` = '%u' WHERE `market_id` = '%u'",
		market->buyer_id, market->price, market->market_id);

	if (SQL_SUCCESS != stmt.PrepareStr(StringBuf_Value(&buf))
	||  SQL_SUCCESS != stmt.BindParam(0, SQLDT_STRING, market->buyer_name, strnlen(market->buyer_name, NAME_LENGTH))
	||  SQL_SUCCESS != stmt.Execute()) {
		SqlStmt_ShowDebug(stmt);
	}
}

void inter_market_fromsql(void) {
	char* data;
	time_t now = time(nullptr);
	t_tick tick = gettick();

	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `market_id`,`seller_id`,`seller_name`,`buyer_id`,`buyer_name`,`price`,`buynow`,`bid_step`,`end_time`,`nameid`,`item_name`,`type`,`refine`,`attribute`,`unique_id`,`enchantgrade`,`card0`,`card1`,`card2`,`card3`,`option_id0`,`option_val0`,`option_parm0`,`option_id1`,`option_val1`,`option_parm1`,`option_id2`,`option_val2`,`option_parm2`,`option_id3`,`option_val3`,`option_parm3`,`option_id4`,`option_val4`,`option_parm4` FROM `custom_market`")) {
		Sql_ShowDebug(sql_handle);
		return;
	}

	while (SQL_SUCCESS == Sql_NextRow(sql_handle)) {
		auto market = std::make_shared<struct market_data>();
		Sql_GetData(sql_handle, 0, &data, nullptr); market->market_id = atoi(data);
		Sql_GetData(sql_handle, 1, &data, nullptr); market->seller_id = atoi(data);
		Sql_GetData(sql_handle, 2, &data, nullptr); safestrncpy(market->seller_name, data, NAME_LENGTH);
		Sql_GetData(sql_handle, 3, &data, nullptr); market->buyer_id = atoi(data);
		Sql_GetData(sql_handle, 4, &data, nullptr); safestrncpy(market->buyer_name, data, NAME_LENGTH);
		Sql_GetData(sql_handle, 5, &data, nullptr); market->price = (uint32)strtoul(data, nullptr, 10);
		Sql_GetData(sql_handle, 6, &data, nullptr); market->buynow = (uint32)strtoul(data, nullptr, 10);
		Sql_GetData(sql_handle, 7, &data, nullptr); market->bid_step = (uint32)strtoul(data, nullptr, 10);
		Sql_GetData(sql_handle, 8, &data, nullptr); market->timestamp = (time_t)atoll(data);

		struct item* item = &market->item;
		Sql_GetData(sql_handle, 9, &data, nullptr); item->nameid = (t_itemid)strtoul(data, nullptr, 10);
		Sql_GetData(sql_handle, 10, &data, nullptr); safestrncpy(market->item_name, data, ITEM_NAME_LENGTH);
		Sql_GetData(sql_handle, 11, &data, nullptr); market->type = atoi(data);
		Sql_GetData(sql_handle, 12, &data, nullptr); item->refine = atoi(data);
		Sql_GetData(sql_handle, 13, &data, nullptr); item->attribute = atoi(data);
		Sql_GetData(sql_handle, 14, &data, nullptr); item->unique_id = strtoull(data, nullptr, 10);
		Sql_GetData(sql_handle, 15, &data, nullptr); item->enchantgrade = atoi(data);

		for (int i = 0; i < 4; i++) {
			Sql_GetData(sql_handle, 16 + i, &data, nullptr);
			item->card[i] = (t_itemid)strtoul(data, nullptr, 10);
		}

		for (int i = 0; i < 5; i++) {
			Sql_GetData(sql_handle, 20 + i*3, &data, nullptr); item->option[i].id = atoi(data);
			Sql_GetData(sql_handle, 21 + i*3, &data, nullptr); item->option[i].value = atoi(data);
			Sql_GetData(sql_handle, 22 + i*3, &data, nullptr); item->option[i].param = atoi(data);
		}

		item->identify = 1;
		item->amount = 1;

		t_tick end_tick;
		if (market->timestamp > now)
			end_tick = (t_tick)(market->timestamp - now) * 1000 + tick;
		else
			end_tick = tick + 5000;

		market->market_end_timer = add_timer(end_tick, market_end_timer, market->market_id, 0);
		market_db[market->market_id] = market;
	}
	Sql_FreeResult(sql_handle);
}

TIMER_FUNC(market_end_timer) {
	auto market = util::umap_find(market_db, (uint32)id);
	if (market == nullptr) return 0;

	if (market->buyer_id > 0) {
		// Calculate 6% tax
		uint32 tax = (uint32)((uint64)market->price * 6 / 100);
		uint32 seller_profit = market->price - tax;

		// Deliver item to buyer
		mail_sendmail(0, "Market System", market->buyer_id, market->buyer_name, "Market: Purchase Won", "You won the auction!", 0, &market->item, 1);
		// Deliver Zeny to seller
		mail_sendmail(0, "Market System", market->seller_id, market->seller_name, "Market: Item Sold", "Your item has been sold.", seller_profit, nullptr, 0);
	} else {
		// Return item to seller
		mail_sendmail(0, "Market System", market->seller_id, market->seller_name, "Market: No Bidders", "Your auction ended with no bidders.", 0, &market->item, 1);
	}

	market->market_end_timer = INVALID_TIMER;
	market_delete(market);
	return 0;
}

// Communication from map-server
void mapif_Market_register(int32 fd, struct market_data* market) {
	StringBuf buf;
	SqlStmt stmt{ *sql_handle };
	int32 j;

	// Use Transaction for Anti-Dupe
	Sql_QueryStr(sql_handle, "START TRANSACTION");

	StringBuf_Init(&buf);
	StringBuf_Printf(&buf, "INSERT INTO `custom_market` (`seller_id`,`seller_name`,`price`,`buynow`,`bid_step`,`end_time`,`nameid`,`item_name`,`type`,`refine`,`attribute`,`unique_id`,`enchantgrade`,`card0`,`card1`,`card2`,`card3`"
		",`option_id0`,`option_val0`,`option_parm0`,`option_id1`,`option_val1`,`option_parm1`,`option_id2`,`option_val2`,`option_parm2`,`option_id3`,`option_val3`,`option_parm3`,`option_id4`,`option_val4`,`option_parm4`"
		") VALUES ('%d',?,'%u','%u','%u','%lu','%u',?,'%d','%d','%d','%" PRIu64 "','%d'",
		market->seller_id, market->price, market->buynow, market->bid_step, (unsigned long)market->timestamp, market->item.nameid, market->type, market->item.refine, market->item.attribute, market->item.unique_id, market->item.enchantgrade);

	for(j = 0; j < 4; j++) StringBuf_Printf(&buf, ",'%u'", market->item.card[j]);
	for(j = 0; j < 5; j++) {
		StringBuf_Printf(&buf, ",'%d'", market->item.option[j].id);
		StringBuf_Printf(&buf, ",'%d'", market->item.option[j].value);
		StringBuf_Printf(&buf, ",'%d'", market->item.option[j].param);
	}
	StringBuf_AppendStr(&buf, ")");

	if (SQL_SUCCESS != stmt.PrepareStr(StringBuf_Value(&buf))
	||  SQL_SUCCESS != stmt.BindParam(0, SQLDT_STRING, market->seller_name, strnlen(market->seller_name, NAME_LENGTH))
	||  SQL_SUCCESS != stmt.BindParam(1, SQLDT_STRING, market->item_name, strnlen(market->item_name, ITEM_NAME_LENGTH))
	||  SQL_SUCCESS != stmt.Execute()) {
		SqlStmt_ShowDebug(stmt);
		Sql_QueryStr(sql_handle, "ROLLBACK");
		// Notify failure to map
		return;
	}

	Sql_QueryStr(sql_handle, "COMMIT");
	market->market_id = (uint32)stmt.LastInsertId();

	auto market_ptr = std::make_shared<struct market_data>();
	memcpy(market_ptr.get(), market, sizeof(struct market_data));

	t_tick duration = (t_tick)(market->timestamp - time(nullptr)) * 1000;
	market_ptr->market_end_timer = add_timer(gettick() + duration, market_end_timer, market->market_id, 0);
	market_db[market->market_id] = market_ptr;
}

void mapif_parse_Market_bid(int32 fd) {
	uint32 char_id = RFIFOL(fd, 2);
	uint32 market_id = RFIFOL(fd, 6);
	uint32 bid_amount = RFIFOL(fd, 10);
	char bidder_name[NAME_LENGTH];
	safestrncpy(bidder_name, (char*)RFIFOP(fd, 14), NAME_LENGTH);

	auto market = util::umap_find(market_db, market_id);
	if (market == nullptr) return;

	if (bid_amount < market->price + market->bid_step && bid_amount < market->buynow) return;

	// Transaction for bidding
	Sql_QueryStr(sql_handle, "START TRANSACTION");

	// Refund previous bidder
	if (market->buyer_id > 0) {
		mail_sendmail(0, "Market System", market->buyer_id, market->buyer_name, "Market: Outbid", "You have been outbid. Zeny returned.", market->price, nullptr, 0);
	}

	market->buyer_id = char_id;
	safestrncpy(market->buyer_name, bidder_name, NAME_LENGTH);
	market->price = bid_amount;

	if (market->buynow > 0 && bid_amount >= market->buynow) {
		market->price = market->buynow;
		// Instant win
		uint32 tax = (uint32)((uint64)market->price * 6 / 100);
		mail_sendmail(0, "Market System", market->buyer_id, market->buyer_name, "Market: Purchase Success", "Instant buy-now success!", 0, &market->item, 1);
		mail_sendmail(0, "Market System", market->seller_id, market->seller_name, "Market: Item Sold", "Your item was bought instantly.", market->price - tax, nullptr, 0);

		Sql_QueryStr(sql_handle, "COMMIT");
		market_delete(market);
	} else {
		market_save(market);
		Sql_QueryStr(sql_handle, "COMMIT");
	}
}

int32 inter_market_parse_frommap(int32 fd) {
	switch (RFIFOW(fd, 0)) {
		case 0x3060: // Market Register
			mapif_Market_register(fd, (struct market_data*)RFIFOP(fd, 4));
			break;
		case 0x3061: // Market Bid
			mapif_parse_Market_bid(fd);
			break;
		default:
			return 0;
	}
	return 1;
}

int32 inter_market_sql_init(void) {
	inter_market_fromsql();
	return 1;
}

void inter_market_sql_final(void) {
	market_db.clear();
}
