//**************************************************************//
// NGHam protocol - Jon Petter Skagmo, LA3JPA, 2014.            //
// Licensed under LGPL.                                         //
//**************************************************************//

#include "ngham.h"

#include "fec-3.0.1/char.h"			// Reed Solomon code from Phil Karn
#include "fec-3.0.1/rs-common.h"
#include <stddef.h>					// For NULL etc.
#include <string.h>					// For memcpy
#include "ccsds_scrambler.h"		// Pre-generated array from scrambling polynomial
#include "ngham_packets.h"			// Structs for TX and RX packets
#include "crc_ccitt.h"
#include "ngham_platform.h"				// CRC-code

// There are seven different sizes.
// Each size has a correlation tag for size, a total size, a maximum payload size and a parity data size.
#define NGH_SIZES	7
const uint8_t NGH_PL_SIZE[] =      {28,   60,   92,   124,  156,  188,  220};		// Actual payload
const uint8_t NGH_PL_SIZE_FULL[] = {31,   63,   95,   127,  159,  191,  223};		// Size with LEN, payload and CRC
const uint8_t NGH_PL_PAR_SIZE[] =  {47,   79,   111,  159,  191,  223,  255};		// Size with RS parity added
const uint8_t NGH_PAR_SIZE[] =     {16,   16,   16,   32,   32,   32,   32};

// Decoder states
#define NGH_STATE_SIZE_TAG		0
#define NGH_STATE_SIZE_TAG_2	1
#define NGH_STATE_SIZE_TAG_3	2
#define NGH_STATE_SIZE_KNOWN	3
#define NGH_STATE_STATUS		4
#define NGH_STATE_STATUS_2		5

// The seven different size tag vectors
const uint32_t NGH_SIZE_TAG[] = {
0b001110110100100111001101,
0b010011011101101001010111,
0b011101101001001110011010,
0b100110111011010010101110,
0b101000001111110101100011,
0b110101100110111011111001,
0b111011010010011100110100
};

// Maximum number of errors in the size tag
#define NGH_SIZE_TAG_MAX_ERROR	6

// Preamble and synchronization vector
const uint8_t NGH_PREAMBLE = 0xAA;
const uint8_t NGH_SYNC[] = {0x5D, 0xE6, 0x2A, 0x7E};

// Reed Solomon control blocks for the different NGHAM sizes
struct rs rs_cb[NGH_SIZES];

void ngham_init(void){
	decoder_state = NGH_STATE_SIZE_TAG;
}

// Run only once - occupies space for reed solomon tables
void ngham_init_arrays(void){
	// Must be de-inited to release occupied space!
	// MM=8, genpoly=0x187, fcs=112, prim=11, nroots=32 eller 16
	struct rs* rs_32 = (void*)init_rs_char(8, 0x187, 112, 11, 32, 0);
	memcpy ((void*)&rs_cb[6], (void*)rs_32, sizeof (rs_cb[6]));
	memcpy ((void*)&rs_cb[5], (void*)rs_32, sizeof (rs_cb[5]));
	memcpy ((void*)&rs_cb[4], (void*)rs_32, sizeof (rs_cb[4]));
	memcpy ((void*)&rs_cb[3], (void*)rs_32, sizeof (rs_cb[3]));

	struct rs* rs_16 = (void*)init_rs_char(8, 0x187, 112, 11, 16, 0);
	memcpy ((void*)&rs_cb[2], (void*)rs_16, sizeof (rs_cb[2]));
	memcpy ((void*)&rs_cb[1], (void*)rs_16, sizeof (rs_cb[1]));
	memcpy ((void*)&rs_cb[0], (void*)rs_16, sizeof (rs_cb[0]));

	rs_cb[6].pad = 255-NGH_PL_PAR_SIZE[6];
	rs_cb[5].pad = 255-NGH_PL_PAR_SIZE[5];
	rs_cb[4].pad = 255-NGH_PL_PAR_SIZE[4];
	rs_cb[3].pad = 255-NGH_PL_PAR_SIZE[3];
	rs_cb[2].pad = 255-NGH_PL_PAR_SIZE[2];
	rs_cb[1].pad = 255-NGH_PL_PAR_SIZE[1];
	rs_cb[0].pad = 255-NGH_PL_PAR_SIZE[0];
}

// Used to check if hamming distance in size tag is smaller than treshold
uint8_t ngham_tag_check(uint32_t x, uint32_t y){
	uint8_t j, distance;
	uint32_t diff;
	diff = x^y;
	if (!diff) return 1;	// Early check to save time

	distance = 0;
	for (j=0; j<24; j++){
		if (diff & 0x01){
			distance++;
			if (distance > NGH_SIZE_TAG_MAX_ERROR) return 0;
		}
		diff >>= 1;
	}
	return 1;
}

// Packets to be transmitted are passed to this function - max. length 220 B
void ngham_encode(tx_pkt_t* p){
	uint16_t j;
	uint16_t crc;
	uint8_t size_nr = 0;
	uint8_t d[NGH_MAX_TOT_SIZE];
	uint16_t d_len = 0;

	// Check size and find control block for smallest possible RS codeword
	if ((p->pl_len == 0) || (p->pl_len > NGH_PL_SIZE[NGH_SIZES-1])) return;
	while (p->pl_len > NGH_PL_SIZE[size_nr]) size_nr++;

	// Insert preamble, sync and size-tag
	for (j=0; j<NGH_PREAMBLE_SIZE; j++) d[d_len++] = NGH_PREAMBLE;
	for (j=0; j<NGH_SYNC_SIZE; j++) d[d_len++] = NGH_SYNC[j];
	d[d_len++] = (NGH_SIZE_TAG[size_nr] >> 16) & 0xff;
	d[d_len++] = (NGH_SIZE_TAG[size_nr] >> 8) & 0xff;
	d[d_len++] = NGH_SIZE_TAG[size_nr] & 0xff;

	// Prepare content of codeword
	d[d_len] = (NGH_PL_SIZE[size_nr] - p->pl_len) & 0x1f;	// Insert padding size
	d[d_len] |= (p->ngham_flags << 5) & 0xe0;				// Insert flags
	d_len++;
	for (j=0; j<p->pl_len; j++) d[d_len++] = p->pl[j];		// Insert data
	crc = crc_ccitt(&d[NGH_CODEWORD_START], p->pl_len+1);	// Insert CRC
	d[d_len++] = (crc >> 8) & 0xff;
	d[d_len++] = crc & 0xff;
	for (j=p->pl_len+3; j<NGH_PL_SIZE_FULL[size_nr]; j++) d[d_len++] = 0;	// Insert padding
	
	// Generate parity data
	encode_rs_char(&rs_cb[size_nr], &d[NGH_CODEWORD_START], &d[d_len]);
	d_len += NGH_PAR_SIZE[size_nr];

	// Scramble
	for (j=0; j<NGH_PL_PAR_SIZE[size_nr]; j++) d[NGH_CODEWORD_START+j] ^= ccsds_poly[j];

	ngham_action_send_data(d, d_len, p->priority);
}

void ngham_decode(uint8_t d){
	static uint8_t size_nr;
	static uint32_t size_tag;

	switch (decoder_state){

		case NGH_STATE_SIZE_TAG:
			size_tag = 0;
			ngham_action_reception_started();
			
		case NGH_STATE_SIZE_TAG_2:
			size_tag <<= 8;
			size_tag |= d;
			decoder_state++;
			break;

		case NGH_STATE_SIZE_TAG_3:
			size_tag <<= 8;
			size_tag |= d;
			{
				uint8_t j;
				for (j=0; j<NGH_SIZES; j++){
					// If tag is intact, set known size
					if (ngham_tag_check(size_tag, NGH_SIZE_TAG[j])){
						size_nr = j;
						decoder_state = NGH_STATE_SIZE_KNOWN;
						rx_buf_len = 0;

						// Set new packet size as soon as possible
						ngham_action_set_packet_size(NGH_PL_PAR_SIZE[size_nr]+NGH_SIZE_TAG_SIZE);
						break;
					}
				}
				// If size tag is not found, every size can theoretically be attempted
				if (decoder_state != NGH_STATE_SIZE_KNOWN){
					ngham_action_handle_packet(PKT_CONDITION_PREFAIL, NULL);
					decoder_state = NGH_STATE_SIZE_TAG;
				}
			}
			break;

		case NGH_STATE_SIZE_KNOWN:
			// De-scramble byte and append to buffer
			rx_buf[rx_buf_len] = d^ccsds_poly[rx_buf_len];
			rx_buf_len++;

			// Do whatever is necessary in this action
			if (rx_buf_len == NGHAM_BYTES_TILL_ACTION_HALFWAY) ngham_action_reception_halfway();

			if (rx_buf_len == NGH_PL_PAR_SIZE[size_nr]){
				int8_t errors;

				// Set packet size back to a large value
				ngham_action_set_packet_size(255);
				decoder_state = NGH_STATE_SIZE_TAG;

				// Run Reed Solomon decoding, calculate packet length
				errors = decode_rs_char(&rs_cb[size_nr], rx_buf, 0, 0);
				rx_pkt.pl_len = NGH_PL_SIZE[size_nr] - (rx_buf[0] & 0x1F);

				// Check if the packet is decodeable and then if CRC is OK - todo: Check length
				if ( (errors != -1) &&
					 (crc_ccitt(rx_buf, rx_pkt.pl_len+1) == ((rx_buf[rx_pkt.pl_len+1]<<8) | rx_buf[rx_pkt.pl_len+2])) ){

					// Copy content to packet struct and pass along
					rx_pkt.errors = errors;
					rx_pkt.ngham_flags = (rx_buf[0] >> 5) & 0x07;
					rx_pkt.noise = ngham_action_get_noise_floor();
					rx_pkt.rssi = ngham_action_get_rssi();
					uint8_t pos;
					for (pos=0; pos<rx_pkt.pl_len; pos++){
						rx_pkt.pl[pos] = rx_buf[pos+1];
					}
					ngham_action_handle_packet(PKT_CONDITION_OK, &rx_pkt);
				}
				// If packet decoding not was successful, count this as an error
				else ngham_action_handle_packet(PKT_CONDITION_FAIL, NULL);


			}
			break;
	}
}
