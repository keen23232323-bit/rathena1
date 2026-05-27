// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "searchstore.hpp"  // struct s_search_store_info

#include <common/cbasetypes.hpp>
#include <common/malloc.hpp>  // aMalloc, aRealloc, aFree
#include <common/showmsg.hpp>  // ShowError, ShowWarning
#include <common/strlib.hpp>  // safestrncpy

#include "battle.hpp"  // battle_config.*
#include "clif.hpp"  // clif_open_search_store_info, clif_search_store_info_*
#include "intif.hpp"
#include "pc.hpp"  // map_session_data

/// Type for shop search function
typedef bool (*searchstore_search_t)( const map_session_data* sd, t_itemid nameid );
typedef bool (*searchstore_searchall_t)( const map_session_data* sd, const struct s_search_store_search* s );

/**
 * Retrieves search function by type.
 * @param type : type of search to conduct
 * @return : search type
 */
static searchstore_search_t searchstore_getsearchfunc(e_searchstore_searchtype type)
{
	switch( type ) {
		case SEARCHTYPE_VENDING:      return &vending_search;
		case SEARCHTYPE_BUYING_STORE: return &buyingstore_search;
	}

	return nullptr;
}

/**
 * Retrieves search-all function by type.
 * @param type : type of search to conduct
 * @return : search type
 */
static searchstore_searchall_t searchstore_getsearchallfunc(e_searchstore_searchtype type)
{
	switch( type ) {
		case SEARCHTYPE_VENDING:      return &vending_searchall;
		case SEARCHTYPE_BUYING_STORE: return &buyingstore_searchall;
	}

	return nullptr;
}

/**
 * Checks if the player has a store by type.
 * @param sd : player requesting
 * @param type : type of search to conduct
 * @return : store type
 */
static bool searchstore_hasstore( const map_session_data& sd, e_searchstore_searchtype type )
{
	switch( type ) {
		case SEARCHTYPE_VENDING:      return sd.state.vending;
		case SEARCHTYPE_BUYING_STORE: return sd.state.buyingstore;
	}

	return false;
}

/**
 * Returns player's store ID by type.
 * @param sd : player requesting
 * @param type : type of search to conduct
 * @return : store ID
 */
static int32 searchstore_getstoreid( const map_session_data& sd, e_searchstore_searchtype type )
{
	switch( type ) {
		case SEARCHTYPE_VENDING:      return sd.vender_id;
		case SEARCHTYPE_BUYING_STORE: return sd.buyer_id;
	}

	return 0;
}

/**
 * Send request to open Search Store.
 * @param sd : player requesting
 * @param uses : uses left to open
 * @param effect : shop type
 * @return : true : opened, false : failed to open
 */
bool searchstore_open(map_session_data& sd, uint16 uses, e_searchstore_effecttype effect, int16 mapid)
{
	if( sd.searchstore.open )
		return false;


	sd.searchstore.open   = true;
	sd.searchstore.uses   = uses;
	sd.searchstore.effect = effect;
	sd.searchstore.mapid  = mapid;

	clif_open_search_store_info(sd);

	return true;
}

/**
 * Query and present the results for the item.
 * @param sd : player requesting
 * @param type : shop type
 * @param min_price : minimum zeny price
 * @param max_price : maximum zeny price
 * @param itemlist : list with stored item results
 * @param item_count : amount of items in itemlist
 * @param cardlist : list with stored cards (cards attached to items)
 * @param card_count : amount of items in cardlist
 */
void searchstore_query(map_session_data& sd, e_searchstore_searchtype type, uint32 min_price, uint32 max_price, const struct PACKET_CZ_SEARCH_STORE_INFO_item* itemlist, uint32 item_count, const struct PACKET_CZ_SEARCH_STORE_INFO_item* cardlist, uint32 card_count)
{
	uint32 i;
	map_session_data* pl_sd;
	struct DBIterator *iter;
	struct s_search_store_search s;
	searchstore_searchall_t store_searchall;
	time_t querytime;

	if( !sd.searchstore.open )
		return;

	if( ( store_searchall = searchstore_getsearchallfunc(type) ) == nullptr ) {
		ShowError("searchstore_query: Unknown search type %u (account_id=%d).\n", type, sd.id);
		return;
	}

	time(&querytime);

	if( sd.searchstore.nextquerytime > querytime ) {
		clif_search_store_info_failed(sd, SSI_FAILED_LIMIT_SEARCH_TIME);
		return;
	}

	if( !sd.searchstore.uses ) {
		clif_search_store_info_failed(sd, SSI_FAILED_SEARCH_CNT);
		return;
	}

	// uses counter must be updated before validating the next search
	sd.searchstore.uses--;
	sd.searchstore.type = type;
	sd.searchstore.nextquerytime = querytime + battle_config.searchstore_querydelay;

	// drop previous results
	searchstore_clear(sd);

	// validate lists
	for( i = 0; i < item_count; i++ ) {
		if( !item_db.exists(itemlist[i].itemId) ) {
			ShowWarning("searchstore_query: Client resolved item %u is not known.\n", itemlist[i].itemId);
			clif_search_store_info_failed(sd, SSI_FAILED_NOTHING_SEARCH_ITEM);

			// update uses
			clif_search_store_info_ack(sd);
			return;
		}
	}
	for( i = 0; i < card_count; i++ ) {
		if( !item_db.exists(cardlist[i].itemId) ) {
			ShowWarning("searchstore_query: Client resolved card %u is not known.\n", cardlist[i].itemId);
			clif_search_store_info_failed(sd, SSI_FAILED_NOTHING_SEARCH_ITEM);

			// update uses
			clif_search_store_info_ack(sd);
			return;
		}
	}

	if( max_price < min_price )
		std::swap(min_price, max_price);

	// search
	s.search_sd  = &sd;
	s.itemlist   = itemlist;
	s.cardlist   = cardlist;
	s.item_count = item_count;
	s.card_count = card_count;
	s.min_price  = min_price;
	s.max_price  = max_price;
	iter         = db_iterator((type == SEARCHTYPE_VENDING) ? vending_getdb() : buyingstore_getdb());

	for( pl_sd = (map_session_data*)dbi_first(iter); dbi_exists(iter);  pl_sd = (map_session_data*)dbi_next(iter) ) {
		if( &sd == pl_sd ) // skip own shop, if any
			continue;

		// Skip stores that are not in the map defined by the search
		if (sd.searchstore.mapid != 0 && pl_sd->m != sd.searchstore.mapid) {
			continue;
		}

		if( !store_searchall(pl_sd, &s) ) { // exceeded result size
			clif_search_store_info_failed(sd, SSI_FAILED_OVER_MAXCOUNT);
			break;
		}
	}

	dbi_destroy(iter);

	// Custom Market Search
	if (type == SEARCHTYPE_VENDING && sd.searchstore.items.size() < (uint32)battle_config.searchstore_maxresults) {
		std::string item_ids_str = "";
		for (i = 0; i < item_count; i++) {
			item_ids_str += std::to_string(itemlist[i].itemId);
			if (i < item_count - 1) item_ids_str += ",";
		}

		if (!item_ids_str.empty()) {
			if (SQL_ERROR != Sql_Query(mmysql_handle,
				"SELECT market_id, seller_id, seller_account, seller_name, nameid, price, refine, enchantgrade, card0, card1, card2, card3, amount "
				"FROM custom_market "
				"WHERE nameid IN (%s) "
				"AND end_time > UNIX_TIMESTAMP() "
				"AND (price >= %u AND price <= %u) "
				"LIMIT %d",
				item_ids_str.c_str(), min_price, max_price, battle_config.searchstore_maxresults - (int)sd.searchstore.items.size()))
			{
				while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
					char* data;
					t_itemid nameid;
					uint32 seller_id;
					Sql_GetData(mmysql_handle, 1, &data, nullptr); seller_id = (uint32)atoi(data);
					Sql_GetData(mmysql_handle, 4, &data, nullptr); nameid = (t_itemid)strtoul(data, nullptr, 10);

					// Card filter logic
					t_itemid cards[MAX_SLOTS];
					for (int j = 0; j < MAX_SLOTS; j++) {
						Sql_GetData(mmysql_handle, 8 + j, &data, nullptr); cards[j] = (t_itemid)strtoul(data, nullptr, 10);
					}

					if (card_count > 0) {
						bool found = false;
						int slot = itemdb_slots(nameid);
						for (int c = 0; c < slot && cards[c]; c++) {
							for (uint32 cidx = 0; cidx < card_count; cidx++) {
								if (cardlist[cidx].itemId == cards[c]) {
									found = true;
									break;
								}
							}
							if (found) break;
						}
						if (!found) continue;
					}

					auto ssitem = std::make_shared<s_search_store_info_item>();
					Sql_GetData(mmysql_handle, 0, &data, nullptr); ssitem->store_id = (int32)(strtoul(data, nullptr, 10) | CUSTOM_MARKET_STORE_ID_OFFSET);
					Sql_GetData(mmysql_handle, 2, &data, nullptr); ssitem->account_id = (uint32)atoi(data); // seller_account
					Sql_GetData(mmysql_handle, 3, &data, nullptr); safestrncpy(ssitem->store_name, data, sizeof(ssitem->store_name));
					if (strstr(ssitem->store_name, " [Market]") == nullptr) {
						size_t len = strlen(ssitem->store_name);
						if (len + 10 < sizeof(ssitem->store_name))
							safestrncpy(ssitem->store_name + len, " [Market]", sizeof(ssitem->store_name) - len);
					}
					ssitem->nameid = nameid;
					Sql_GetData(mmysql_handle, 5, &data, nullptr); ssitem->price = (uint32)strtoul(data, nullptr, 10);
					Sql_GetData(mmysql_handle, 6, &data, nullptr); ssitem->refine = (unsigned char)atoi(data);
					Sql_GetData(mmysql_handle, 7, &data, nullptr); ssitem->enchantgrade = (uint8)atoi(data);
					for (int j = 0; j < MAX_SLOTS; j++) ssitem->card[j] = cards[j];
					Sql_GetData(mmysql_handle, 12, &data, nullptr); ssitem->amount = (uint16)atoi(data);

					if (seller_id != sd.status.char_id) // Skip own listings
						sd.searchstore.items.push_back(ssitem);

					if (sd.searchstore.items.size() >= (uint32)battle_config.searchstore_maxresults)
						break;
				}
				Sql_FreeResult(mmysql_handle);
			}
		}
	}

	if( !sd.searchstore.items.empty() ) {
		// present results
		clif_search_store_info_ack( sd );

		// one page displayed
		sd.searchstore.pages++;
	} else {
		// cleanup
		searchstore_clear(sd);

		// notify of failure (must go before updating uses)
		clif_search_store_info_failed(sd, SSI_FAILED_NOTHING_SEARCH_ITEM);

		// update uses
		clif_search_store_info_ack( sd );
	}
}

/**
 * Checks whether or not more results are available for the client.
 * @param sd : player requesting
 * @return : true : more items to search, false : no more items
 */
bool searchstore_querynext( const map_session_data& sd )
{
	if( !sd.searchstore.items.empty() && ( sd.searchstore.items.size()-1 )/SEARCHSTORE_RESULTS_PER_PAGE > sd.searchstore.pages )
		return true;

	return false;
}

/**
 * Get and display the results for the next page.
 * @param sd : player requesting
 */
void searchstore_next(map_session_data& sd)
{
	if( !sd.searchstore.open || sd.searchstore.items.size() <= sd.searchstore.pages*SEARCHSTORE_RESULTS_PER_PAGE ) // nothing (more) to display
		return;

	// present results
	clif_search_store_info_ack( sd );

	// one more page displayed
	sd.searchstore.pages++;
}

/**
 * Prepare to clear information for closing of window.
 * @param sd : player requesting
 */
void searchstore_clear(map_session_data& sd)
{
	searchstore_clearremote(sd);

	sd.searchstore.items.clear();
	sd.searchstore.pages = 0;
}

/**
 * Close the Search Store window.
 * @param sd : player requesting
 */
void searchstore_close(map_session_data& sd)
{
	if( sd.searchstore.open ) {
		searchstore_clear(sd);

		sd.searchstore.uses = 0;
		sd.searchstore.open = false;
	}
}

/**
 * Process the actions (click) for the Search Store window.
 * @param sd : player requesting
 * @param account_id : account ID of owner's shop
 * @param store_id : store ID created by client
 * @param nameid : item being searched
 */
void searchstore_click(map_session_data& sd, uint32 account_id, int32 store_id, t_itemid nameid)
{
	uint32 i;
	map_session_data* pl_sd;
	searchstore_search_t store_search;

	if( !sd.searchstore.open || sd.searchstore.items.empty() )
		return;

	searchstore_clearremote(sd);

	ARR_FIND( 0, sd.searchstore.items.size(), i, sd.searchstore.items[i]->store_id == store_id && sd.searchstore.items[i]->account_id == account_id && sd.searchstore.items[i]->nameid == nameid );
	if( i == sd.searchstore.items.size() ) { // no such result, crafted
		ShowWarning("searchstore_click: Received request with item %u of account %d, which is not part of current result set (account_id=%d, char_id=%d).\n", nameid, account_id, sd.id, sd.status.char_id);
		clif_search_store_info_failed(sd, SSI_FAILED_SSILIST_CLICK_TO_OPEN_STORE);
		return;
	}

	if (store_id & CUSTOM_MARKET_STORE_ID_OFFSET) {
		uint32 market_id = store_id & ~CUSTOM_MARKET_STORE_ID_OFFSET;
		uint32 price = sd.searchstore.items[i]->price;

		if ((uint32)sd.status.zeny < price) {
			clif_displaymessage(sd.fd, "Market: You do not have enough Zeny.");
			return;
		}

		intif_Market_bid(sd.status.char_id, market_id, price, sd.status.name);
		return;
	}

	if( ( pl_sd = map_id2sd(account_id) ) == nullptr ) { // no longer online
		clif_search_store_info_failed(sd, SSI_FAILED_SSILIST_CLICK_TO_OPEN_STORE);
		return;
	}

	if( !searchstore_hasstore(*pl_sd, sd.searchstore.type) || searchstore_getstoreid(*pl_sd, sd.searchstore.type) != store_id ) { // no longer vending/buying or not same shop
		clif_search_store_info_failed(sd, SSI_FAILED_SSILIST_CLICK_TO_OPEN_STORE);
		return;
	}

	store_search = searchstore_getsearchfunc(sd.searchstore.type);

	if( !store_search(pl_sd, nameid) ) {// item no longer being sold/bought
		clif_search_store_info_failed(sd, SSI_FAILED_SSILIST_CLICK_TO_OPEN_STORE);
		return;
	}

	switch( sd.searchstore.effect ) {
		case SEARCHSTORE_EFFECT_NORMAL:
			// display coords
			if( sd.m != pl_sd->m ) // not on same map, wipe previous marker
				clif_search_store_info_click_ack(sd, -1, -1);
			else
				clif_search_store_info_click_ack(sd, pl_sd->x, pl_sd->y);
			break;
		case SEARCHSTORE_EFFECT_REMOTE:
			// open remotely
			// to bypass range checks
			sd.searchstore.remote_id = account_id;

			switch( sd.searchstore.type ) {
				case SEARCHTYPE_VENDING:      vending_vendinglistreq(&sd, account_id); break;
				case SEARCHTYPE_BUYING_STORE: buyingstore_open(&sd, account_id);       break;
			}
			break;
		default:
			// unknown
			ShowError("searchstore_click: Unknown search store effect %u (account_id=%d).\n", sd.searchstore.effect, sd.id);
	}
}

/**
 * Checks whether or not sd has opened account_id's shop remotely.
 * @param sd : player requesting
 * @param account_id : account ID of owner's shop
 * @return : true : shop opened, false : shop not opened
 */
bool searchstore_queryremote( const map_session_data& sd, uint32 account_id )
{
	return (bool)( sd.searchstore.open && !sd.searchstore.items.empty() && sd.searchstore.remote_id == account_id );
}

/**
 * Removes range-check bypassing for remotely opened stores.
 * @param sd : player requesting
 */
void searchstore_clearremote(map_session_data& sd)
{
	sd.searchstore.remote_id = 0;
}
