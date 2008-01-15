///////////////////////////////////////////////////////////////////////////////
//
/// \file       price_table_gen.c
/// \brief      Probability price table generator
///
/// Compiling: gcc -std=c99 -o price_table_gen price_table_gen.c
//
//  Copyright (C) 2007 Lasse Collin
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2.1 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
///////////////////////////////////////////////////////////////////////////////

#include <sys/types.h>
#include <inttypes.h>
#include <stdio.h>
#include "range_common.h"
#include "price_table_init.c"


int
main(void)
{
	lzma_rc_init();

	printf("/* This file has been automatically generated by "
			"price_table_gen.c. */\n\n"
			"#include \"range_encoder.h\"\n\n"
			"const uint32_t lzma_rc_prob_prices["
			"BIT_MODEL_TOTAL >> MOVE_REDUCING_BITS] = {");

	const size_t array_size = sizeof(lzma_rc_prob_prices)
			/ sizeof(lzma_rc_prob_prices[0]);
	for (size_t i = 0; i < array_size; ++i) {
		if (i % 8 == 0)
			printf("\n\t");

		printf("% 4" PRIu32, lzma_rc_prob_prices[i]);

		if (i != array_size - 1)
			printf(",");
	}

	printf("\n};\n");

	return 0;
}