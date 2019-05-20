#ifndef FL2_DATA_BLOCK_H_
#define FL2_DATA_BLOCK_H_


typedef struct {
	const uint8_t* data;
	size_t start;
	size_t end;
} lzma_data_block;


#endif /* FL2_DATA_BLOCK_H_ */