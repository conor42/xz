///////////////////////////////////////////////////////////////////////////////
//
/// \file       data_block.h
/// \brief      Radix match-finder overlapped data block
///
//  Author:     Conor McCarthy
//
//  This source code is licensed under both the BSD-style license (found in the
//  LICENSE file in the root directory of this source tree) and the GPLv2 (found
//  in the COPYING file in the root directory of this source tree).
//  You may select, at your option, one of the above-listed licenses.
//
///////////////////////////////////////////////////////////////////////////////

#ifndef LZMA_DATA_BLOCK_H
#define LZMA_DATA_BLOCK_H


typedef struct {
	const uint8_t *data;
	size_t start;
	size_t end;
} lzma_data_block;


#endif