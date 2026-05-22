// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef INT_MARKET_HPP
#define INT_MARKET_HPP

#include <common/cbasetypes.hpp>
#include <common/mmo.hpp>

struct market_data;

int32 inter_market_parse_frommap(int32 fd);
int32 inter_market_sql_init(void);
void inter_market_sql_final(void);

#endif /* INT_MARKET_HPP */
