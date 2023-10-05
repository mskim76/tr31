/**
 * @file tr31.c
 * @brief High level TR-31 library interface
 *
 * Copyright (c) 2020, 2021, 2022, 2023 Leon Lynch
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "tr31.h"
#include "tr31_config.h"
#include "tr31_crypto.h"

#include "crypto_tdes.h"
#include "crypto_aes.h"
#include "crypto_mem.h"
#include "crypto_rand.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#if defined(HAVE_ARPA_INET_H)
#include <arpa/inet.h> // for ntohs and friends
#elif defined(HAVE_WINSOCK_H)
#include <winsock.h>
#endif

#define sizeof_field(TYPE, FIELD) sizeof(((TYPE*)0)->FIELD)

// TR-31 header
// see ANSI X9.143:2021, 6.2, table 1
struct tr31_header_t {
	uint8_t version_id;
	char length[4];
	uint16_t key_usage;
	uint8_t algorithm;
	uint8_t mode_of_use;
	char key_version[2];
	uint8_t exportability;
	char opt_blocks_count[2];
	char reserved[2];
} __attribute__((packed));

// TR-31 optional block header with short length
// see ANSI X9.143:2021, 6.2, table 1
struct tr31_opt_blk_hdr_t {
	uint16_t id;
	char length[2] NONSTRING;
} __attribute__((packed));

// TR-31 optional block header with extended length
// see ANSI X9.143:2021, 6.2, table 1
struct tr31_opt_blk_hdr_ext_t {
	uint16_t id;
	char reserved[2];
	char ext_length_byte_count[2] NONSTRING;
	char ext_length[] NONSTRING;
} __attribute__((packed));

// TR-31 optional block with short length
// see ANSI X9.143:2021, 6.2, table 1
struct tr31_opt_blk_t {
	uint16_t id;
	char length[2] NONSTRING;
	char data[];
} __attribute__((packed));

// TR-31 payload
// see ANSI X9.143:2021, 6.1, figure 2
// see ANSI X9.143:2021, 7.3.1, figure 11 and table 26
struct tr31_payload_t {
	uint16_t length;
	uint8_t data[];
} __attribute__((packed));

#define TR31_MIN_PAYLOAD_LENGTH (DES_BLOCK_SIZE)
#define TR31_MIN_KEY_BLOCK_LENGTH (sizeof(struct tr31_header_t) + TR31_MIN_PAYLOAD_LENGTH + 8) // Minimum TR-31 key block length: header + minimum payload + authenticator

// helper functions
static int dec_to_int(const char* str, size_t str_len);
static void int_to_dec(unsigned int value, char* str, size_t str_len);
static int hex_to_int(const char* str, size_t str_len);
static void int_to_hex(unsigned int value, char* str, size_t str_len);
static int hex_to_bin(const char* hex, size_t hex_len, void* bin, size_t bin_len);
static int bin_to_hex(const void* bin, size_t bin_len, char* str, size_t str_len);
static int tr31_validate_format_an(const char* buf, size_t buf_len);
static int tr31_validate_format_h(const char* buf, size_t buf_len);
static int tr31_validate_format_pa(const char* buf, size_t buf_len);
static struct tr31_opt_ctx_t* tr31_opt_block_alloc(struct tr31_ctx_t* ctx, unsigned int id, size_t length);
static inline size_t tr31_opt_block_kcv_data_length(size_t kcv_len);
static int tr31_opt_block_encode_kcv(uint8_t kcv_algorithm, const void* kcv, size_t kcv_len, char* encoded_data, size_t encoded_data_len);
static int tr31_opt_block_validate_hash_algorithm(uint8_t hash_algorithm);
static int tr31_opt_block_parse(const void* ptr, size_t remaining_len, size_t* opt_block_len, struct tr31_opt_ctx_t* opt_ctx);
static int tr31_opt_block_validate_iso8601(const char* ts_str, size_t ts_str_len);
static int tr31_opt_block_export(const struct tr31_opt_ctx_t* opt_ctx, size_t remaining_len, size_t* opt_blk_len, void* ptr);
static int tr31_opt_block_export_PB(const struct tr31_ctx_t* ctx, size_t pb_len, struct tr31_opt_blk_t* opt_blk);
static int tr31_compute_final_lengths(struct tr31_ctx_t* ctx);
static int tr31_tdes_decrypt_verify_variant_binding(struct tr31_ctx_t* ctx, const struct tr31_key_t* kbpk);
static int tr31_tdes_encrypt_sign_variant_binding(struct tr31_ctx_t* ctx, const struct tr31_key_t* kbpk);
static int tr31_tdes_decrypt_verify_derivation_binding(struct tr31_ctx_t* ctx, const struct tr31_key_t* kbpk);
static int tr31_tdes_encrypt_sign_derivation_binding(struct tr31_ctx_t* ctx, const struct tr31_key_t* kbpk);
static int tr31_aes_decrypt_verify_derivation_binding(struct tr31_ctx_t* ctx, const struct tr31_key_t* kbpk);
static int tr31_aes_encrypt_sign_derivation_binding(struct tr31_ctx_t* ctx, const struct tr31_key_t* kbpk);

static int dec_to_int(const char* str, size_t str_len)
{
	int value;

	value = 0;
	for (size_t i = 0; i < str_len; ++i) {
		if (str[i] < '0' && str[i] > '9') {
			return -1;
		}

		value *= 10; // shift decimal value
		value += str[i] - '0'; // convert ASCII decimal to numeric value
	}

	return value;
}

static void int_to_dec(unsigned int value, char* str, size_t str_len)
{
	// pack string digits, right justified
	while (str_len) {
		uint8_t digit;

		digit = value % 10; // extract digit
		value /= 10; // shift decimal value

		str[str_len - 1] = digit + '0'; // convert numeric value to ASCII decimal
		--str_len;
	}
}

static int hex_to_int(const char* str, size_t str_len)
{
	int value;

	value = 0;
	for (size_t i = 0; i < str_len; ++i) {
		value <<= 4; // shift hex value
		// convert ASCII hex to numeric value
		// lower case characters are not allowed
		// see ANSI X9.143:2021, 4
		if (str[i] >= '0' && str[i] <= '9') {
			value += str[i] - '0';
		} else if (str[i] >= 'A' && str[i] <= 'F') {
			value += str[i] - ('A' - 10);
		} else {
			// invalid character
			return -1;
		}
	}

	return value;
}

static void int_to_hex(unsigned int value, char* str, size_t str_len)
{
	// pack string digits, right justified
	while (str_len) {
		uint8_t digit;

		digit = value & 0xF; // extract digit
		value >>= 4; // shift hex value

		// convert numeric value to ASCII hex
		if (digit < 0xA) {
			str[str_len - 1] = digit + '0';
		} else {
			str[str_len - 1] = digit - 0xA + 'A';
		}
		--str_len;
	}
}

static int hex_to_bin(const char* hex, size_t hex_len, void* bin, size_t bin_len)
{
	uint8_t* ptr = bin;

	// even number of hex digits
	if ((hex_len & 0x1) != 0) {
		return -1;
	}

	while (hex_len && bin_len) {
		uint8_t nibble;

		// convert ASCII hex digit to numeric value
		if (*hex >= '0' && *hex <= '9') {
			nibble = *hex - '0';
		} else if (*hex >= 'A' && *hex <= 'F') {
			nibble = *hex - ('A' - 10);
		} else {
			// invalid character
			return -3;
		}

		if ((hex_len & 0x1) == 0) { // even digit index
			// most significant nibble
			*ptr = nibble << 4;
		} else { // i is odd
			// least significant nibble
			*ptr |= nibble & 0x0F;
			++ptr;
			--bin_len;
		}

		++hex;
		--hex_len;
	}

	return 0;
}

static int bin_to_hex(const void* bin, size_t bin_len, char* hex, size_t hex_len)
{
	const uint8_t* buf = bin;

	// minimum string length
	if (hex_len < bin_len * 2) {
		return -1;
	}

	// pack hex digits, left justified
	for (unsigned int i = 0; i < bin_len; ++i) {
		uint8_t digit;

		// convert most significant nibble
		digit = buf[i] >> 4;
		if (digit < 0xA) {
			hex[(i * 2)] = digit + '0';
		} else {
			hex[(i * 2)] = digit - 0xA + 'A';
		}

		// convert least significant nibble
		digit = buf[i] & 0xf;
		if (digit < 0xA) {
			hex[(i * 2) + 1] = digit + '0';
		} else {
			hex[(i * 2) + 1] = digit - 0xA + 'A';
		}
	}

	return 0;
}

static int tr31_validate_format_an(const char* buf, size_t buf_len)
{
	while (buf_len--) {
		// alphanumeric characters are in the ranges 0x30 - 0x39, 0x41 - 0x5A
		// and 0x61 - 0x7A
		// see ANSI X9.143:2021, 4
		if ((*buf < 0x30 || *buf > 0x39) &&
			(*buf < 0x41 || *buf > 0x5A) &&
			(*buf < 0x61 || *buf > 0x7A)
		) {
			return -1;
		}

		++buf;
	}

	return 0;
}

static int tr31_validate_format_h(const char* buf, size_t buf_len)
{
	while (buf_len--) {
		// hex characters are in the ranges 0x30 - 0x39 and 0x41 - 0x46
		// lower case characters are not allowed
		// see ANSI X9.143:2021, 4
		if ((*buf < 0x30 || *buf > 0x39) &&
			(*buf < 0x41 || *buf > 0x46)
		) {
			return -1;
		}

		++buf;
	}

	return 0;
}

static int tr31_validate_format_pa(const char* buf, size_t buf_len)
{
	while (buf_len--) {
		// printable ASCII characters are in the range 0x20 to 0x7E
		// see ANSI X9.143:2021, 4
		if (*buf < 0x20 || *buf > 0x7E) {
			return -1;
		}

		++buf;
	}

	return 0;
}

const char* tr31_lib_version_string(void)
{
	return TR31_LIB_VERSION_STRING;
}

int tr31_key_init(
	unsigned int usage,
	unsigned int algorithm,
	unsigned int mode_of_use,
	const char* key_version,
	unsigned int exportability,
	const void* data,
	size_t length,
	struct tr31_key_t* key
)
{
	int r;

	if (!key_version || !key) {
		return -1;
	}

	memset(key, 0, sizeof(*key));

	// validate key usage field
	// see ANSI X9.143:2021, 6.3.1, table 2
	key->usage = usage;
	switch (usage) {
		case TR31_KEY_USAGE_BDK:
		case TR31_KEY_USAGE_DUKPT_IK:
		case TR31_KEY_USAGE_BKV:
		case TR31_KEY_USAGE_KDK:
		case TR31_KEY_USAGE_CVK:
		case TR31_KEY_USAGE_DATA:
		case TR31_KEY_USAGE_ASYMMETRIC_DATA:
		case TR31_KEY_USAGE_DATA_DEC_TABLE:
		case TR31_KEY_USAGE_DATA_SENSITIVE:
		case TR31_KEY_USAGE_EMV_MKAC:
		case TR31_KEY_USAGE_EMV_MKSMC:
		case TR31_KEY_USAGE_EMV_MKSMI:
		case TR31_KEY_USAGE_EMV_MKDAC:
		case TR31_KEY_USAGE_EMV_MKDN:
		case TR31_KEY_USAGE_EMV_CP:
		case TR31_KEY_USAGE_EMV_OTHER:
		case TR31_KEY_USAGE_EMV_AKP_PIN:
		case TR31_KEY_USAGE_IV:
		case TR31_KEY_USAGE_KEK:
		case TR31_KEY_USAGE_TR31_KBPK:
		case TR31_KEY_USAGE_TR34_APK_KRD:
		case TR31_KEY_USAGE_APK:
		case TR31_KEY_USAGE_ISO20038_KBPK:
		case TR31_KEY_USAGE_ISO16609_MAC_1:
		case TR31_KEY_USAGE_ISO9797_1_MAC_1:
		case TR31_KEY_USAGE_ISO9797_1_MAC_2:
		case TR31_KEY_USAGE_ISO9797_1_MAC_3:
		case TR31_KEY_USAGE_ISO9797_1_MAC_4:
		case TR31_KEY_USAGE_ISO9797_1_MAC_5:
		case TR31_KEY_USAGE_ISO9797_1_CMAC:
		case TR31_KEY_USAGE_HMAC:
		case TR31_KEY_USAGE_ISO9797_1_MAC_6:
		case TR31_KEY_USAGE_PEK:
		case TR31_KEY_USAGE_PGK:
		case TR31_KEY_USAGE_AKP_SIG:
		case TR31_KEY_USAGE_AKP_CA:
		case TR31_KEY_USAGE_AKP_OTHER:
		case TR31_KEY_USAGE_PVK:
		case TR31_KEY_USAGE_PVK_IBM3624:
		case TR31_KEY_USAGE_PVK_VISA_PVV:
		case TR31_KEY_USAGE_PVK_X9_132_ALG_1:
		case TR31_KEY_USAGE_PVK_X9_132_ALG_2:
		case TR31_KEY_USAGE_PVK_X9_132_ALG_3:
			// supported
			break;

		default:
			return TR31_ERROR_UNSUPPORTED_KEY_USAGE;
	}

	// validate algorithm field
	// see ANSI X9.143:2021, 6.3.2, table 3
	key->algorithm = algorithm;
	switch (algorithm) {
		case TR31_KEY_ALGORITHM_AES:
		case TR31_KEY_ALGORITHM_DES:
		case TR31_KEY_ALGORITHM_EC:
		case TR31_KEY_ALGORITHM_HMAC:
		case TR31_KEY_ALGORITHM_RSA:
		case TR31_KEY_ALGORITHM_DSA:
		case TR31_KEY_ALGORITHM_TDES:
			// supported
			break;

		default:
			return TR31_ERROR_UNSUPPORTED_ALGORITHM;
	}

	// validate mode of use field
	// see ANSI X9.143:2021, 6.3.3, table 4
	key->mode_of_use = mode_of_use;
	switch (mode_of_use) {
		case TR31_KEY_MODE_OF_USE_ENC_DEC:
		case TR31_KEY_MODE_OF_USE_MAC:
		case TR31_KEY_MODE_OF_USE_DEC:
		case TR31_KEY_MODE_OF_USE_ENC:
		case TR31_KEY_MODE_OF_USE_MAC_GEN:
		case TR31_KEY_MODE_OF_USE_ANY:
		case TR31_KEY_MODE_OF_USE_SIG:
		case TR31_KEY_MODE_OF_USE_MAC_VERIFY:
		case TR31_KEY_MODE_OF_USE_DERIVE:
		case TR31_KEY_MODE_OF_USE_VARIANT:
			// supported
			break;

		default:
			return TR31_ERROR_UNSUPPORTED_MODE_OF_USE;
	}

	// validate key version number field
	// see ANSI X9.143:2021, 6.3.4, table 5
	r = tr31_key_set_key_version(key, key_version);
	if (r) {
		// return error value as-is
		return r;
	}

	// validate exportability field
	// see ANSI X9.143:2021, 6.3.5, table 6
	key->exportability = exportability;
	switch (exportability) {
		case TR31_KEY_EXPORT_TRUSTED:
		case TR31_KEY_EXPORT_NONE:
		case TR31_KEY_EXPORT_SENSITIVE:
			// supported
			break;

		default:
			return TR31_ERROR_UNSUPPORTED_EXPORTABILITY;
	}

	// if key data is available, copy it
	if (data && length) {
		// validate key length by algorithm
		switch (algorithm) {
			case TR31_KEY_ALGORITHM_TDES:
				if (length > 24) {
					// invalid TDES key length
					return TR31_ERROR_INVALID_KEY_LENGTH;
				}
				break;

			case TR31_KEY_ALGORITHM_AES:
				if (length > 32) {
					// invalid AES key length
					return TR31_ERROR_INVALID_KEY_LENGTH;
				}
				break;
		}

		r = tr31_key_set_data(key, data, length);
		if (r) {
			// return error value as-is
			return r;
		}
	}

	return 0;
}

void tr31_key_release(struct tr31_key_t* key)
{
	if (key->data) {
		crypto_cleanse(key->data, key->length);
		free(key->data);
		key->data = NULL;
	}
}

int tr31_key_copy(
	const struct tr31_key_t* src,
	struct tr31_key_t* key
)
{
	int r;
	char key_version[2];

	if (!src || !key) {
		return -1;
	}

	r = tr31_key_get_key_version(src, key_version);
	if (r) {
		// return error value as-is
		return r;
	}

	return tr31_key_init(
		src->usage,
		src->algorithm,
		src->mode_of_use,
		key_version,
		src->exportability,
		src->data,
		src->length,
		key
	);
}

int tr31_key_set_data(struct tr31_key_t* key, const void* data, size_t length)
{
	int r;

	if (!key || !data || !length) {
		return -1;
	}

	tr31_key_release(key);

	// copy key data
	key->length = length;
	key->data = calloc(1, key->length);
	memcpy(key->data, data, key->length);

	// update KCV
	key->kcv_len = 0;
	memset(&key->kcv, 0, sizeof(key->kcv));
	if (key->algorithm == TR31_KEY_ALGORITHM_TDES) {
		// use legacy KCV for TDES key
		// see ANSI X9.24-1:2017, 7.7.2
		key->kcv_algorithm = TR31_OPT_BLOCK_KCV_LEGACY;
		r = crypto_tdes_kcv_legacy(key->data, key->length, key->kcv);
		if (r) {
			// failed to compute KCV
			return TR31_ERROR_KCV_NOT_AVAILABLE;
		}
		key->kcv_len = DES_KCV_SIZE_LEGACY;
		return 0;

	} else if (key->algorithm == TR31_KEY_ALGORITHM_AES) {
		// use CMAC-based KCV for AES key
		// see ANSI X9.24-1:2017, 7.7.2
		key->kcv_algorithm = TR31_OPT_BLOCK_KCV_CMAC;
		r = crypto_aes_kcv(key->data, key->length, key->kcv);
		if (r) {
			// failed to compute KCV
			return TR31_ERROR_KCV_NOT_AVAILABLE;
		}
		key->kcv_len = AES_KCV_SIZE;
		return 0;
	}

	// key algorithm not suitable for KCV computation; continue
	return 0;
}

int tr31_key_set_key_version(struct tr31_key_t* key, const char* key_version)
{
	int r;

	if (!key || !key_version) {
		return -1;
	}

	// decode key version number field
	// see ANSI X9.143:2021, 6.3.4, table 5
	r = tr31_validate_format_an(key_version, 2);
	if (r) {
		return TR31_ERROR_INVALID_KEY_VERSION_FIELD;
	}
	if (key_version[0] == '0' && key_version[1] == '0') {
		key->key_version = TR31_KEY_VERSION_IS_UNUSED;
		memset(key->key_version_str, 0, sizeof(key->key_version_str));
	} else if (key_version[0] == 'c') {
		key->key_version = TR31_KEY_VERSION_IS_COMPONENT;
		memcpy(key->key_version_str, key_version, 2);
		key->key_version_str[2] = 0;
	} else {
		key->key_version = TR31_KEY_VERSION_IS_VALID;
		memcpy(key->key_version_str, key_version, 2);
		key->key_version_str[2] = 0;
	}

	return 0;
}

int tr31_key_get_key_version(const struct tr31_key_t* key, char* key_version)
{
	if (!key || !key_version) {
		return -1;
	}

	// encode key version number field
	// see ANSI X9.143:2021, 6.3.4, table 5
	switch (key->key_version) {
		case TR31_KEY_VERSION_IS_UNUSED:
			memset(key_version, '0', sizeof_field(struct tr31_header_t, key_version));
			break;

		case TR31_KEY_VERSION_IS_COMPONENT:
			key_version[0] = 'c';
			key_version[1] = key->key_version_str[1];
			break;

		case TR31_KEY_VERSION_IS_VALID:
			memcpy(key_version, key->key_version_str, sizeof_field(struct tr31_header_t, key_version));
			break;

		default:
			return TR31_ERROR_INVALID_KEY_VERSION_FIELD;
	}

	return 0;
}

int tr31_init(
	uint8_t version_id,
	const struct tr31_key_t* key,
	struct tr31_ctx_t* ctx
)
{
	int r;

	if (!ctx) {
		return -1;
	}
	memset(ctx, 0, sizeof(*ctx));

	// validate key block format
	ctx->version = version_id;
	switch (ctx->version) {
		case TR31_VERSION_A:
		case TR31_VERSION_B:
		case TR31_VERSION_C:
		case TR31_VERSION_D:
		case TR31_VERSION_E:
			// supported
			break;

		default:
			return TR31_ERROR_UNSUPPORTED_VERSION;
	}

	// copy key, if available
	if (key) {
		r = tr31_key_copy(key, &ctx->key);
		if (r) {
			// return error value as-is
			return r;
		}
	}

	return 0;
}

static struct tr31_opt_ctx_t* tr31_opt_block_alloc(
	struct tr31_ctx_t* ctx,
	unsigned int id,
	size_t length
)
{
	struct tr31_opt_ctx_t* opt_ctx;

	if (!ctx) {
		return NULL;
	}

	// repeated optional block IDs are not allowed
	// see ANSI X9.143:2021, 6.3.6
	opt_ctx = tr31_opt_block_find(ctx, id);
	if (opt_ctx) {
		// existing optional block found
		return NULL;
	}

	// grow optional block array
	ctx->opt_blocks_count++;
	ctx->opt_blocks = realloc(ctx->opt_blocks, ctx->opt_blocks_count * sizeof(struct tr31_opt_ctx_t));

	// copy optional block fields and allocate optional block data
	opt_ctx = &ctx->opt_blocks[ctx->opt_blocks_count - 1];
	opt_ctx->id = id;
	opt_ctx->data_length = length;
	if (length) {
		opt_ctx->data = malloc(opt_ctx->data_length);
	} else {
		opt_ctx->data = NULL;
	}

	return opt_ctx;
}

int tr31_opt_block_add(
	struct tr31_ctx_t* ctx,
	unsigned int id,
	const void* data,
	size_t length
)
{
	struct tr31_opt_ctx_t* opt_ctx;

	if (!ctx) {
		return -1;
	}
	if (!data && length) {
		return -2;
	}

	opt_ctx = tr31_opt_block_alloc(ctx, id, length);
	if (!opt_ctx) {
		return -3;
	}

	if (data && length) {
		// copy optional block data
		memcpy(opt_ctx->data, data, length);
	}

	return 0;
}

struct tr31_opt_ctx_t* tr31_opt_block_find(struct tr31_ctx_t* ctx, unsigned int id)
{
	if (!ctx) {
		return NULL;
	}

	for (size_t i = 0; i < ctx->opt_blocks_count; ++i) {
		if (ctx->opt_blocks[i].id == id) {
			return &ctx->opt_blocks[i];
		}
	}

	return NULL;
}

static inline size_t tr31_opt_block_kcv_data_length(size_t kcv_len)
{
	return (kcv_len + 1) * 2;
}

static int tr31_opt_block_encode_kcv(
	uint8_t kcv_algorithm,
	const void* kcv,
	size_t kcv_len,
	char* encoded_data,
	size_t encoded_data_len
)
{
	int r;
	uint8_t buf[6];
	size_t buf_len;

	if (!kcv || !kcv_len || !encoded_data || !encoded_data_len) {
		return -1;
	}

	// validate KCV length according to KCV algorithm
	// see ANSI X9.143:2021, 6.3.6.7, table 15
	// see ANSI X9.143:2021, 6.3.6.12, table 20
	// KCV lengths should comply with ANSI X9.24-1, Annex A
	if (kcv_algorithm == TR31_OPT_BLOCK_KCV_LEGACY) {
		if (kcv_len > 3) {
			// Legacy KCV should be truncated to 3 bytes or less
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
		}
	} else if (kcv_algorithm == TR31_OPT_BLOCK_KCV_CMAC) {
		if (kcv_len > 5) {
			// CMAC KCV should be truncated to 5 bytes or less
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
		}
	} else {
		// Unknown KCV algorithm
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	buf_len = kcv_len + 1;
	if (buf_len > sizeof(buf)) {
		return -2;
	}
	if (buf_len * 2 > encoded_data_len) {
		return -3;
	}

	buf[0] = kcv_algorithm;
	memcpy(&buf[1], kcv, kcv_len);
	r = bin_to_hex(
		buf,
		buf_len,
		encoded_data,
		encoded_data_len
	);
	if (r) {
		return -4;
	}

	return 0;
}

int tr31_opt_block_decode_kcv(
	const struct tr31_opt_ctx_t* opt_ctx,
	struct tr31_opt_blk_kcv_data_t* kcv_data
)
{
	int r;

	if (!opt_ctx || !kcv_data) {
		return -1;
	}

	// decode optional block data and validate
	// see ANSI X9.143:2021, 6.3.6.7, table 15
	// see ANSI X9.143:2021, 6.3.6.12, table 20
	// KCV lengths should comply with ANSI X9.24-1, Annex A
	if (opt_ctx->data_length < 2 + 1) { // KCV algorithm and at least one KCV digit
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	r = hex_to_bin(
		opt_ctx->data,
		2,
		&kcv_data->kcv_algorithm,
		sizeof(kcv_data->kcv_algorithm)
	);
	if (r) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	switch (kcv_data->kcv_algorithm) {
		case TR31_OPT_BLOCK_KCV_LEGACY:
			// at most 6 KCV digits, thus 3 KCV bytes
			if (opt_ctx->data_length > tr31_opt_block_kcv_data_length(3)) {
				// too many KCV digits for legacy KCV algorithm
				// see ANSI X9.24-1:2017, Annex A
				return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
			}
			break;

		case TR31_OPT_BLOCK_KCV_CMAC:
			// at most 10 KCV digits, thus 5 KCV bytes
			if (opt_ctx->data_length > tr31_opt_block_kcv_data_length(5)) {
				// too many KCV digits for CMAC KCV algorithm
				// see ANSI X9.24-1:2017, Annex A
				return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
			}
			break;

		default:
			// unknown KCV algorithm
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	kcv_data->kcv_len = (opt_ctx->data_length - 2) / 2;
	r = hex_to_bin(
		opt_ctx->data + 2,
		opt_ctx->data_length - 2,
		&kcv_data->kcv,
		kcv_data->kcv_len
	);
	if (r) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	return 0;
}

int tr31_opt_block_add_AL(
	struct tr31_ctx_t* ctx,
	uint8_t akl
)
{
	int r;
	uint8_t buf[2];
	char encoded_data[4];

	if (!ctx) {
		return -1;
	}

	if (akl != TR31_OPT_BLOCK_AL_AKL_EPHEMERAL &&
		akl != TR31_OPT_BLOCK_AL_AKL_STATIC
	) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	// encode optional block data
	// assume AKL optional block version 1
	// see ANSI X9.143:2021, 6.3.6.1, table 8
	buf[0] = TR31_OPT_BLOCK_AL_VERSION_1;
	buf[1] = akl;
	r = bin_to_hex(
		buf,
		sizeof(buf),
		encoded_data,
		sizeof(encoded_data)
	);
	if (r) {
		return -2;
	}

	return tr31_opt_block_add(ctx, TR31_OPT_BLOCK_AL, encoded_data, sizeof(buf) * 2);
}

int tr31_opt_block_decode_AL(
	const struct tr31_opt_ctx_t* opt_ctx,
	struct tr31_opt_blk_akl_data_t* akl_data
)
{
	int r;

	if (!opt_ctx || !akl_data) {
		return -1;
	}

	if (opt_ctx->id != TR31_OPT_BLOCK_AL) {
		return -2;
	}

	// decode optional block data and validate
	// see ANSI X9.143:2021, 6.3.6.1, table 8
	if (opt_ctx->data_length < 2) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	r = hex_to_bin(opt_ctx->data, 2, &akl_data->version, sizeof(akl_data->version));
	if (r) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	if (akl_data->version == TR31_OPT_BLOCK_AL_VERSION_1) {
		uint8_t akl = 0xFF;

		// decode AKL optional block version 1
		if (opt_ctx->data_length != 4) {
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
		}
		r = hex_to_bin(opt_ctx->data + 2, 2, &akl, sizeof(akl));
		if (r) {
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
		}
		if (akl != TR31_OPT_BLOCK_AL_AKL_EPHEMERAL &&
			akl != TR31_OPT_BLOCK_AL_AKL_STATIC
		) {
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
		}
		akl_data->v1.akl = akl;

	} else {
		// unsupported AKL version
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	return 0;
}

int tr31_opt_block_add_BI(
	struct tr31_ctx_t* ctx,
	uint8_t key_type,
	const void* bdkid,
	size_t bdkid_len
)
{
	int r;
	uint8_t buf[6];
	size_t buf_len;
	char encoded_data[12];

	if (!ctx || !bdkid) {
		return -1;
	}

	// validate KSI / BDK-ID length according to key type
	// see ANSI X9.143:2021, 6.3.6.2, table 9
	switch (key_type) {
		case TR31_OPT_BLOCK_BI_TDES_DUKPT:
			if (ctx->key.algorithm != TR31_KEY_ALGORITHM_TDES) {
				return TR31_ERROR_UNSUPPORTED_ALGORITHM;
			}
			if (bdkid_len != 5) {
				return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
			}
			break;

		case TR31_OPT_BLOCK_BI_AES_DUKPT:
			if (ctx->key.algorithm != TR31_KEY_ALGORITHM_AES) {
				return TR31_ERROR_UNSUPPORTED_ALGORITHM;
			}
			if (bdkid_len != 4) {
				return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
			}
			break;

		default:
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	// encode optional block data
	buf[0] = key_type;
	memcpy(&buf[1], bdkid, bdkid_len);
	buf_len = bdkid_len + 1;
	r = bin_to_hex(
		buf,
		buf_len,
		encoded_data,
		sizeof(encoded_data)
	);
	if (r) {
		return -2;
	}

	return tr31_opt_block_add(ctx, TR31_OPT_BLOCK_BI, encoded_data, buf_len * 2);
}

int tr31_opt_block_decode_BI(
	const struct tr31_opt_ctx_t* opt_ctx,
	struct tr31_opt_blk_bdkid_data_t* bdkid_data
)
{
	int r;

	if (!opt_ctx || !bdkid_data) {
		return -1;
	}

	if (opt_ctx->id != TR31_OPT_BLOCK_BI) {
		return -2;
	}

	// decode optional block data and validate
	// see ANSI X9.143:2021, 6.3.6.2, table 9
	r = hex_to_bin(opt_ctx->data, 2, &bdkid_data->key_type, sizeof(bdkid_data->key_type));
	if (r) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	switch (bdkid_data->key_type) {
		case TR31_OPT_BLOCK_BI_TDES_DUKPT:
			if (opt_ctx->data_length != 12) {
				return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
			}

			// 5 bytes for TDES DUKPT
			bdkid_data->bdkid_len = 5;
			break;

		case TR31_OPT_BLOCK_BI_AES_DUKPT:
			if (opt_ctx->data_length != 10) {
				return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
			}

			// 4 bytes for AES DUKPT
			bdkid_data->bdkid_len = 4;
			break;

		default:
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	r = hex_to_bin(opt_ctx->data + 2, opt_ctx->data_length - 2, &bdkid_data->bdkid, bdkid_data->bdkid_len);
	if (r) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	return 0;
}

int tr31_opt_block_add_CT(
	struct tr31_ctx_t* ctx,
	uint8_t cert_format,
	const char* cert_base64,
	size_t cert_base64_len
)
{
	struct tr31_opt_ctx_t* opt_block_ct;

	if (!ctx || !cert_base64) {
		return -1;
	}

	// validate certificate format
	// see ANSI X9.143:2021, 6.3.6.3, table 10
	switch (cert_format) {
		case TR31_OPT_BLOCK_CT_X509:
			break;

		case TR31_OPT_BLOCK_CT_EMV:
			break;

		default:
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	if (cert_base64[cert_base64_len-1] == 0) {
		// if already null-terminated, determine exact string length which
		// must be shorter than the given length due to the null-termination
		cert_base64_len = strlen(cert_base64);
	}

	// find existing optional block CT
	opt_block_ct = NULL;
	for (size_t i = 0; i < ctx->opt_blocks_count; ++i) {
		if (ctx->opt_blocks[i].id == TR31_OPT_BLOCK_CT) {
			opt_block_ct = &ctx->opt_blocks[i];
			break;
		}
	}

	if (opt_block_ct) {
		struct tr31_opt_ctx_t old = *opt_block_ct;
		const char* old_data = old.data;

		if (old.data_length < 2) {
			// existing optional block CT is invalid
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
		}

		// update existing optional block CT

		if ((old_data[0] == '0' && old_data[1] == '0') ||
			(old_data[0] == '0' && old_data[1] == '1')
		) {
			char* data;

			// compute cert chain length
			// - 2 bytes for certificate chain format
			// - 2 bytes for first certificate format (included in first certificate data length)
			// - 4 bytes for first certificate length
			// - first certificate data
			// - 2 bytes for second certificate format (not included in second certificate data length)
			// - 4 bytes for second certificate length
			// - second certificate data
			opt_block_ct->data_length = 2 + 4 + old.data_length + 2 + 4 + cert_base64_len;

			// convert to cert chain
			opt_block_ct->data = malloc(opt_block_ct->data_length);
			data = opt_block_ct->data;
			int_to_hex(TR31_OPT_BLOCK_CT_CERT_CHAIN, data, 2);
			memcpy(data + 2, old.data, 2); // copy first certificate format
			int_to_hex(old.data_length - 2, data + 4, 4); // copy first certificate length
			memcpy(data + 8, old.data + 2, old.data_length - 2);

			// add new cert to chain
			data += 6 + old.data_length;
			int_to_hex(cert_format, data, 2); // copy second certificate format
			int_to_hex(cert_base64_len, data + 2, 4); // copy second certificate length
			memcpy(data + 6, cert_base64, cert_base64_len);

			// cleanup optional block CT data
			free(old.data);
			old.data = NULL;
			old.data_length = 0;

			return 0;

		} else if (old_data[0] == '0' && old_data[1] == '2') {
			char* data;

			// extend existing certificate chain
			// - 2 bytes for next certificate format
			// - 4 bytes for next certificate length
			// - next certificate data
			opt_block_ct->data_length += 2 + 4 + cert_base64_len;
			opt_block_ct->data = realloc(opt_block_ct->data, opt_block_ct->data_length);
			data = opt_block_ct->data + opt_block_ct->data_length - 2 - 4 - cert_base64_len;

			// add new cert to chain
			int_to_hex(cert_format, data, 2); // copy certificate format
			int_to_hex(cert_base64_len, data + 2, 4); // copy certificate length
			memcpy(data + 6, cert_base64, cert_base64_len);

			return 0;

		} else {
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
		}

	} else {
		struct tr31_opt_ctx_t* opt_ctx;

		// add new optional block CT
		// NOTE: tr31_opt_block_export() copies this optional block verbatim
		opt_ctx = tr31_opt_block_alloc(ctx, TR31_OPT_BLOCK_CT, cert_base64_len + 2);
		if (!opt_ctx) {
			return -2;
		}
		int_to_hex(cert_format, opt_ctx->data, 2);
		memcpy(opt_ctx->data + 2, cert_base64, cert_base64_len);

		return 0;
	}
}

int tr31_opt_block_add_DA(
	struct tr31_ctx_t* ctx,
	const void* da,
	size_t da_len
)
{
	int r;
	struct tr31_opt_ctx_t* opt_ctx;

	if (!ctx || !da) {
		return -1;
	}

	if (!da_len || (da_len % 5 != 0)) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	// validate as alphanumeric (format AN)
	// see ANSI X9.143:2021, 6.3.6.4, table 12
	r = tr31_validate_format_an(da, da_len);
	if (r) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	opt_ctx = tr31_opt_block_alloc(ctx, TR31_OPT_BLOCK_DA, da_len + 2);
	if (!opt_ctx) {
		return -2;
	}
	int_to_hex(TR31_OPT_BLOCK_DA_VERSION_1, opt_ctx->data, 2);
	memcpy(opt_ctx->data + 2, da, da_len);

	return r;
}

int tr31_opt_block_decode_DA(
	const struct tr31_opt_ctx_t* opt_ctx,
	struct tr31_opt_blk_da_data_t* da_data,
	size_t da_data_len
)
{
	size_t count;
	const uint8_t* da_attr;

	if (!opt_ctx || !da_data || !da_data_len) {
		return -1;
	}

	if (opt_ctx->id != TR31_OPT_BLOCK_DA) {
		return -2;
	}

	// decode optional block DA version
	// see ANSI X9.143:2021, 6.3.6.1, table 8
	if (opt_ctx->data_length < 2) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	da_data->version = hex_to_int(opt_ctx->data, 2);
	if (da_data->version != TR31_OPT_BLOCK_DA_VERSION_1) {
		// unsupported DA version
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	// validate optional block length
	if (opt_ctx->data_length < 7 ||
		(opt_ctx->data_length - 2) % 5 != 0
	) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	count = (opt_ctx->data_length - 2) / 5;

	// validate output data length
	if (da_data_len != sizeof(struct tr31_opt_blk_da_attr_t) * count + sizeof(struct tr31_opt_blk_da_data_t)) {
		return -3;
	}

	// decode optional block DA version 1
	// see ANSI X9.143:2021, 6.3.6.1, table 8
	da_attr = opt_ctx->data + 2;
	for (size_t i = 0; i < count; ++i) {
		uint16_t key_usage_raw = da_attr[0];
		key_usage_raw += da_attr[1] << 8;
		da_data->attr[i].key_usage = ntohs(key_usage_raw);
		da_data->attr[i].algorithm = da_attr[2];
		da_data->attr[i].mode_of_use = da_attr[3];
		da_data->attr[i].exportability = da_attr[4];
		da_attr += 5;
	}

	return 0;
}

static int tr31_opt_block_validate_hash_algorithm(uint8_t hash_algorithm)
{
	// validate hash algorithm
	// see ANSI X9.143:2021, 6.3.6.5, table 13
	switch (hash_algorithm) {
		case TR31_OPT_BLOCK_HM_SHA1:
		case TR31_OPT_BLOCK_HM_SHA224:
		case TR31_OPT_BLOCK_HM_SHA256:
		case TR31_OPT_BLOCK_HM_SHA384:
		case TR31_OPT_BLOCK_HM_SHA512:
		case TR31_OPT_BLOCK_HM_SHA512_224:
		case TR31_OPT_BLOCK_HM_SHA512_256:
		case TR31_OPT_BLOCK_HM_SHA3_224:
		case TR31_OPT_BLOCK_HM_SHA3_256:
		case TR31_OPT_BLOCK_HM_SHA3_384:
		case TR31_OPT_BLOCK_HM_SHA3_512:
		case TR31_OPT_BLOCK_HM_SHAKE128:
		case TR31_OPT_BLOCK_HM_SHAKE256:
			// valid
			return 0;

		default:
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
}

int tr31_opt_block_add_HM(
	struct tr31_ctx_t* ctx,
	uint8_t hash_algorithm
)
{
	int r;
	char encoded_data[2];

	if (!ctx) {
		return -1;
	}

	// validate hash algorithm
	// see ANSI X9.143:2021, 6.3.6.5, table 13
	r = tr31_opt_block_validate_hash_algorithm(hash_algorithm);
	if (r) {
		// return error value as-is
		return r;
	}

	// encode optional block data
	r = bin_to_hex(
		&hash_algorithm,
		sizeof(hash_algorithm),
		encoded_data,
		sizeof(encoded_data)
	);
	if (r) {
		return -2;
	}

	return tr31_opt_block_add(ctx, TR31_OPT_BLOCK_HM, encoded_data, sizeof(encoded_data));
}

int tr31_opt_block_decode_HM(
	const struct tr31_opt_ctx_t* opt_ctx,
	uint8_t* hash_algorithm
)
{
	int r;

	if (!opt_ctx || !hash_algorithm) {
		return -1;
	}

	if (opt_ctx->id != TR31_OPT_BLOCK_HM) {
		return -2;
	}

	// decode optional block data and validate
	// see ANSI X9.143:2021, 6.3.6.5, table 13
	if (opt_ctx->data_length != 2) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	r = hex_to_bin(opt_ctx->data, 2, hash_algorithm, sizeof(*hash_algorithm));
	if (r) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	r = tr31_opt_block_validate_hash_algorithm(*hash_algorithm);
	if (r) {
		// return error value as-is
		return r;
	}

	return 0;
}

int tr31_opt_block_add_IK(
	struct tr31_ctx_t* ctx,
	const void* ikid,
	size_t ikid_len
)
{
	int r;
	char encoded_data[16];

	if (!ctx || !ikid) {
		return -1;
	}

	// IKID must be 8 bytes (thus 16 hex digits)
	// see ANSI X9.143:2021, 6.3.6.6, table 14
	if (ikid_len != 8) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	// encode optional block data
	r = bin_to_hex(
		ikid,
		ikid_len,
		encoded_data,
		sizeof(encoded_data)
	);
	if (r) {
		return -2;
	}

	return tr31_opt_block_add(ctx, TR31_OPT_BLOCK_IK, encoded_data, sizeof(encoded_data));
}

int tr31_opt_block_decode_IK(
	const struct tr31_opt_ctx_t* opt_ctx,
	void* ikid,
	size_t ikid_len
)
{
	int r;

	if (!opt_ctx || !ikid) {
		return -1;
	}

	if (opt_ctx->id != TR31_OPT_BLOCK_IK) {
		return -2;
	}

	// IKID must be 8 bytes (thus 16 hex digits)
	// see ANSI X9.143:2021, 6.3.6.6, table 14
	if (ikid_len != 8) {
		return -3;
	}
	if (opt_ctx->data_length != 16) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	r = hex_to_bin(opt_ctx->data, opt_ctx->data_length, ikid, ikid_len);
	if (r) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	return 0;
}

int tr31_opt_block_add_KC(struct tr31_ctx_t* ctx)
{
	// add empty KC optional block to be finalised by tr31_export()
	return tr31_opt_block_add(ctx, TR31_OPT_BLOCK_KC, NULL, 0);
}

int tr31_opt_block_decode_KC(
	const struct tr31_opt_ctx_t* opt_ctx,
	struct tr31_opt_blk_kcv_data_t* kcv_data
)
{
	if (!opt_ctx || !kcv_data) {
		return -1;
	}

	if (opt_ctx->id != TR31_OPT_BLOCK_KC) {
		return -2;
	}

	return tr31_opt_block_decode_kcv(opt_ctx, kcv_data);
}

int tr31_opt_block_add_KP(struct tr31_ctx_t* ctx)
{
	// add empty KP optional block to be finalised by tr31_export()
	return tr31_opt_block_add(ctx, TR31_OPT_BLOCK_KP, NULL, 0);
}

int tr31_opt_block_decode_KP(
	const struct tr31_opt_ctx_t* opt_ctx,
	struct tr31_opt_blk_kcv_data_t* kcv_data
)
{
	if (!opt_ctx || !kcv_data) {
		return -1;
	}

	if (opt_ctx->id != TR31_OPT_BLOCK_KP) {
		return -2;
	}

	return tr31_opt_block_decode_kcv(opt_ctx, kcv_data);
}

int tr31_opt_block_add_KS(
	struct tr31_ctx_t* ctx,
	const void* iksn,
	size_t iksn_len
)
{
	int r;
	char encoded_data[20];

	if (!ctx || !iksn) {
		return -1;
	}

	// IKSN must be 10 bytes (thus 20 hex digits)
	// see ANSI X9.143:2021, 6.3.6.8, table 16
	// NOTE: this implementation also allows 8 bytes (thus 16 hex digits) for
	// compatibility with other legacy implementations
	if (iksn_len != 10 && iksn_len != 8) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	// encode optional block data
	r = bin_to_hex(
		iksn,
		iksn_len,
		encoded_data,
		sizeof(encoded_data)
	);
	if (r) {
		return -2;
	}

	return tr31_opt_block_add(ctx, TR31_OPT_BLOCK_KS, encoded_data, iksn_len * 2);
}

int tr31_opt_block_decode_KS(
	const struct tr31_opt_ctx_t* opt_ctx,
	void* iksn,
	size_t iksn_len
)
{
	int r;

	if (!opt_ctx || !iksn) {
		return -1;
	}

	if (opt_ctx->id != TR31_OPT_BLOCK_KS) {
		return -2;
	}

	// IKSN must be 10 bytes (thus 20 hex digits)
	// see ANSI X9.143:2021, 6.3.6.8, table 16
	// NOTE: this implementation also allows 8 bytes (thus 16 hex digits) for
	// compatibility with other legacy implementations
	if (iksn_len != 10 && iksn_len != 8) {
		return -3;
	}
	if (opt_ctx->data_length != 20 && opt_ctx->data_length != 16) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	r = hex_to_bin(opt_ctx->data, opt_ctx->data_length, iksn, iksn_len);
	if (r) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	if (iksn_len == 10 && opt_ctx->data_length == 16) {
		// zero last two bytes if there is a length mismatch
		memset(iksn + 8, 0, 2);
	}

	return 0;
}

int tr31_opt_block_add_KV(
	struct tr31_ctx_t* ctx,
	const char* version_id,
	const char* other
)
{
	uint8_t buf[4];

	if (!ctx) {
		return -1;
	}

	// see ANSI X9.143:2021, 6.3.6.9, table 17
	memset(buf, 0x30, sizeof(buf)); // default value
	if (version_id) {
		memcpy(buf, version_id, 2);
	}
	if (other) {
		memcpy(buf + 2, other, 2);
	}

	return tr31_opt_block_add(ctx, TR31_OPT_BLOCK_KV, buf, sizeof(buf));
}

int tr31_opt_block_add_LB(
	struct tr31_ctx_t* ctx,
	const char* label
)
{
	int r;

	if (!ctx || !label) {
		return -1;
	}

	// validate as printable ASCII (format PA)
	// see ANSI X9.143:2021, 6.3.6.10, table 18
	r = tr31_validate_format_pa(label, strlen(label));
	if (r) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	return tr31_opt_block_add(ctx, TR31_OPT_BLOCK_LB, label, strlen(label));
}

int tr31_opt_block_add_PK(
	struct tr31_ctx_t* ctx,
	uint8_t kcv_algorithm,
	const void* kcv,
	size_t kcv_len
)
{
	int r;
	char encoded_data[12];

	if (!ctx || !kcv) {
		return -1;
	}

	r = tr31_opt_block_encode_kcv(
		kcv_algorithm,
		kcv,
		kcv_len,
		encoded_data,
		sizeof(encoded_data)
	);
	if (r) {
		// return error value as-is
		return r;
	}

	return tr31_opt_block_add(
		ctx,
		TR31_OPT_BLOCK_PK,
		encoded_data,
		tr31_opt_block_kcv_data_length(kcv_len)
	);
}

int tr31_opt_block_decode_PK(
	const struct tr31_opt_ctx_t* opt_ctx,
	struct tr31_opt_blk_kcv_data_t* kcv_data
)
{
	if (!opt_ctx || !kcv_data) {
		return -1;
	}

	if (opt_ctx->id != TR31_OPT_BLOCK_PK) {
		return -2;
	}

	return tr31_opt_block_decode_kcv(opt_ctx, kcv_data);
}

int tr31_opt_block_add_TC(
	struct tr31_ctx_t* ctx,
	const char* tc_str
)
{
	int r;

	if (!ctx || !tc_str) {
		return -1;
	}

	// validate date/time string
	r = tr31_opt_block_validate_iso8601(tc_str, strlen(tc_str));
	if (r) {
		// return error value as-is
		return r;
	}

	return tr31_opt_block_add(ctx, TR31_OPT_BLOCK_TC, tc_str, strlen(tc_str));
}

int tr31_opt_block_add_TS(
	struct tr31_ctx_t* ctx,
	const char* ts_str
)
{
	int r;

	if (!ctx || !ts_str) {
		return -1;
	}

	// validate date/time string
	r = tr31_opt_block_validate_iso8601(ts_str, strlen(ts_str));
	if (r) {
		// return error value as-is
		return r;
	}

	return tr31_opt_block_add(ctx, TR31_OPT_BLOCK_TS, ts_str, strlen(ts_str));
}

int tr31_opt_block_add_WP(
	struct tr31_ctx_t* ctx,
	uint8_t wrapping_pedigree
)
{
	char buf[3];

	if (wrapping_pedigree > 3) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	// assume wrapping pedigree optional block version 00
	// see ANSI X9.143:2021, 6.3.6.15, table 23
	int_to_hex(TR31_OPT_BLOCK_WP_VERSION_0, buf, 2);
	int_to_hex(wrapping_pedigree, buf + 2, 1);
	return tr31_opt_block_add(ctx, TR31_OPT_BLOCK_WP, buf, sizeof(buf));
}

int tr31_opt_block_decode_WP(
	const struct tr31_opt_ctx_t* opt_ctx,
	struct tr31_opt_blk_wp_data_t* wp_data
)
{
	int r;

	if (!opt_ctx || !wp_data) {
		return -1;
	}

	if (opt_ctx->id != TR31_OPT_BLOCK_WP) {
		return -2;
	}

	// decode optional block data and validate
	// see ANSI X9.143:2021, 6.3.6.15, table 23
	if (opt_ctx->data_length < 2) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	r = hex_to_bin(opt_ctx->data, 2, &wp_data->version, sizeof(wp_data->version));
	if (r) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}
	if (wp_data->version == TR31_OPT_BLOCK_WP_VERSION_0) {
		uint8_t wrapping_pedigree;

		// decode WP optional block version 00
		if (opt_ctx->data_length != 3) {
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
		}
		wrapping_pedigree = hex_to_int(opt_ctx->data + 2, 1);
		if (wrapping_pedigree > 3) {
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
		}
		wp_data->v0.wrapping_pedigree = wrapping_pedigree;

	} else {
		// unsupported AKL version
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	return 0;
}

int tr31_import(
	const char* key_block,
	const struct tr31_key_t* kbpk,
	struct tr31_ctx_t* ctx
)
{
	int r;
	size_t key_block_len;
	const struct tr31_header_t* header;
	size_t opt_blk_len_total = 0;
	unsigned int enc_block_size;
	const void* ptr;

	if (!key_block || !ctx) {
		return -1;
	}

	key_block_len = strlen(key_block);
	header = (const struct tr31_header_t*)key_block;

	// validate minimum length
	if (key_block_len < TR31_MIN_KEY_BLOCK_LENGTH) {
		return TR31_ERROR_INVALID_LENGTH;
	}

	// initialise TR-31 context object
	r = tr31_init(header->version_id, NULL, ctx);
	if (r) {
		// return error value as-is
		return r;
	}

	// decode key block length field
	ctx->length = dec_to_int(header->length, sizeof(header->length));
	if (ctx->length != key_block_len) {
		return TR31_ERROR_INVALID_LENGTH_FIELD;
	}

	// decode header fields associated with wrapped key
	r = tr31_key_init(
		ntohs(header->key_usage),
		header->algorithm,
		header->mode_of_use,
		header->key_version,
		header->exportability,
		NULL,
		0,
		&ctx->key
	);
	if (r) {
		tr31_key_release(&ctx->key);
		// return error value as-is
		return r;
	}

	// decode number of optional blocks field
	int opt_blocks_count = dec_to_int(header->opt_blocks_count, sizeof(header->opt_blocks_count));
	if (opt_blocks_count < 0) {
		return TR31_ERROR_INVALID_NUMBER_OF_OPTIONAL_BLOCKS_FIELD;
	}
	ctx->opt_blocks_count = opt_blocks_count;

	// decode optional blocks
	// see ANSI X9.143:2021, 6.3.6
	ptr = header + 1; // optional blocks, if any, are after the header
	if (ctx->opt_blocks_count) {
		ctx->opt_blocks = calloc(ctx->opt_blocks_count, sizeof(ctx->opt_blocks[0]));
	}
	for (int i = 0; i < opt_blocks_count; ++i) {
		// ensure that current pointer is valid for minimal optional block
		if (ptr + sizeof(struct tr31_opt_blk_t) - (void*)header > key_block_len) {
			r = TR31_ERROR_INVALID_LENGTH;
			goto error;
		}

		// copy optional block field
		size_t opt_blk_len;
		r = tr31_opt_block_parse(
			ptr,
			(void*)key_block + key_block_len - ptr,
			&opt_blk_len,
			&ctx->opt_blocks[i]
		);
		if (r) {
			// return error value as-is
			goto error;
		}

		// compute total optional block length
		opt_blk_len_total += opt_blk_len;

		// advance current pointer
		ptr += opt_blk_len;
	}

	// validate key block format version
	// set associated authenticator length
	// set encryption block size for header length validation
	switch (ctx->version) {
		case TR31_VERSION_A:
		case TR31_VERSION_C:
			ctx->authenticator_length = 4; // 4 bytes; 8 ASCII hex digits
			enc_block_size = DES_BLOCK_SIZE;
			break;

		case TR31_VERSION_B:
			ctx->authenticator_length = 8; // 8 bytes; 16 ASCII hex digits
			enc_block_size = DES_BLOCK_SIZE;
			break;

		case TR31_VERSION_D:
			ctx->authenticator_length = 16; // 16 bytes; 32 ASCII hex digits
			enc_block_size = AES_BLOCK_SIZE;
			break;

		case TR31_VERSION_E:
			ctx->authenticator_length = 16; // 16 bytes; 32 ASCII hex digits
			enc_block_size = AES_BLOCK_SIZE;
			break;

		default:
			// invalid format version
			return -1;
	}

	// ANSI X9.143:2021, 6.3.6 (page 19) indicates that the padding block must
	// result in the total length of all optional blocks being a multiple of
	// the encryption block length.
	// ISO 20038:2017, A.2.1 (page 10) indicates that the total length of all
	// optional blocks must be a multiple of the encryption block size and
	// does not make an exception for format version E.
	// So we'll use the encryption block size which is determined by the TR-31
	// format version.
	if (opt_blk_len_total & (enc_block_size-1)) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	// ensure that current pointer is valid for minimal payload and authenticator
	if (ptr - (void*)header + TR31_MIN_PAYLOAD_LENGTH + (ctx->authenticator_length * 2) > key_block_len) {
		r = TR31_ERROR_INVALID_LENGTH;
		goto error;
	}

	// update header data in context object
	ctx->header_length = ptr - (void*)header;
	ctx->header = (void*)header;

	// determine payload length
	size_t key_block_payload_length = key_block_len - ctx->header_length - (ctx->authenticator_length * 2);
	ctx->payload_length = key_block_payload_length / 2;

	// add payload data to context object
	ctx->payload = calloc(1, ctx->payload_length);
	r = hex_to_bin(ptr, key_block_payload_length, ctx->payload, ctx->payload_length);
	if (r) {
		r = TR31_ERROR_INVALID_PAYLOAD_FIELD;
		goto error;
	}
	ptr += key_block_payload_length;

	// ensure that current point is valid for remaining authenticator
	if (ptr - (void*)header + (ctx->authenticator_length * 2) != key_block_len) {
		r = TR31_ERROR_INVALID_LENGTH;
		goto error;
	}

	// add authenticator to context object
	ctx->authenticator = calloc(1, ctx->authenticator_length);
	r = hex_to_bin(ptr, ctx->authenticator_length * 2, ctx->authenticator, ctx->authenticator_length);
	if (r) {
		r = TR31_ERROR_INVALID_AUTHENTICATOR_FIELD;
		goto error;
	}

	// if no key block protection key was provided, we are done
	if (!kbpk) {
		r = 0;
		goto exit;
	}

	switch (ctx->version) {
		case TR31_VERSION_A:
		case TR31_VERSION_B:
		case TR31_VERSION_C: {
			// only allow TDES key block protection keys
			if (kbpk->algorithm != TR31_KEY_ALGORITHM_TDES) {
				r = TR31_ERROR_UNSUPPORTED_KBPK_ALGORITHM;
				goto error;
			}

			// validate payload length
			// ANSI X9.143:2021 requires key length obfuscation padding up to
			// the maximum key length for the algorithm while TR-31:2018 does
			// not appear to indicate a minimum or maximum for key length
			// padding, and therefore this implementation only enforces the
			// cipher block size
			if (ctx->payload_length & (DES_BLOCK_SIZE-1)) {
				// payload length must be a multiple of TDES block size
				// for format version A, B, C
				r = TR31_ERROR_INVALID_KEY_LENGTH;
				goto error;
			}

			if (ctx->version == TR31_VERSION_A || ctx->version == TR31_VERSION_C) {
				// verify and decrypt payload
				r = tr31_tdes_decrypt_verify_variant_binding(ctx, kbpk);
			} else if (ctx->version == TR31_VERSION_B) {
				// decrypt and verify payload
				r = tr31_tdes_decrypt_verify_derivation_binding(ctx, kbpk);
			} else {
				// invalid format version
				return -1;
			}
			if (r) {
				// return error value as-is
				goto error;
			}

			// validate payload length field
			switch (ctx->key.algorithm) {
				case TR31_KEY_ALGORITHM_TDES:
					if (ctx->key.length != TDES2_KEY_SIZE &&
						ctx->key.length != TDES3_KEY_SIZE
					) {
						r = TR31_ERROR_INVALID_KEY_LENGTH;
						goto error;
					}
					break;

				default:
					// unsupported; continue
					break;
			}

			break;
		}

		case TR31_VERSION_D: {
			// only allow AES key block protection keys
			if (kbpk->algorithm != TR31_KEY_ALGORITHM_AES) {
				r = TR31_ERROR_UNSUPPORTED_KBPK_ALGORITHM;
				goto error;
			}

			// validate payload length
			// ANSI X9.143:2021 requires key length obfuscation padding up to
			// the maximum key length for the algorithm while neither
			// TR-31:2018 nor ISO 20038:2017 appear to indicate a minimum or
			// maximum for key length padding, and therefore this
			// implementation only enforces the cipher block size
			if (ctx->payload_length & (AES_BLOCK_SIZE-1)) {
				// payload length must be a multiple of AES block size
				// for format version D
				r = TR31_ERROR_INVALID_KEY_LENGTH;
				goto error;
			}

			// decrypt and verify payload
			r = tr31_aes_decrypt_verify_derivation_binding(ctx, kbpk);
			if (r) {
				// return error value as-is
				goto error;
			}

			// validate payload length field
			switch (ctx->key.algorithm) {
				case TR31_KEY_ALGORITHM_TDES:
					if (ctx->key.length != TDES2_KEY_SIZE &&
						ctx->key.length != TDES3_KEY_SIZE
					) {
						r = TR31_ERROR_INVALID_KEY_LENGTH;
						goto error;
					}
					break;

				case TR31_KEY_ALGORITHM_AES:
					if (ctx->key.length != AES128_KEY_SIZE &&
						ctx->key.length != AES192_KEY_SIZE &&
						ctx->key.length != AES256_KEY_SIZE
					) {
						r = TR31_ERROR_INVALID_KEY_LENGTH;
						goto error;
					}
					break;

				default:
					// unsupported; continue
					break;
			}

			break;
		}

		case TR31_VERSION_E: {
			// only allow AES key block protection keys
			if (kbpk->algorithm != TR31_KEY_ALGORITHM_AES) {
				r = TR31_ERROR_UNSUPPORTED_KBPK_ALGORITHM;
				goto error;
			}

			// decrypt and verify payload
			r = tr31_aes_decrypt_verify_derivation_binding(ctx, kbpk);
			if (r) {
				// return error value as-is
				goto error;
			}

			// validate payload length field
			switch (ctx->key.algorithm) {
				case TR31_KEY_ALGORITHM_TDES:
					if (ctx->key.length != TDES2_KEY_SIZE &&
						ctx->key.length != TDES3_KEY_SIZE
					) {
						r = TR31_ERROR_INVALID_KEY_LENGTH;
						goto error;
					}
					break;

				case TR31_KEY_ALGORITHM_AES:
					if (ctx->key.length != AES128_KEY_SIZE &&
						ctx->key.length != AES192_KEY_SIZE &&
						ctx->key.length != AES256_KEY_SIZE
					) {
						r = TR31_ERROR_INVALID_KEY_LENGTH;
						goto error;
					}
					break;

				default:
					// unsupported; continue
					break;
			}

			break;
		}

		default:
			// invalid format version
			return -1;
	}

	r = 0;
	goto exit;

error:
	tr31_release(ctx);
exit:
	return r;
}

int tr31_export(
	struct tr31_ctx_t* ctx,
	const struct tr31_key_t* kbpk,
	char* key_block,
	size_t key_block_len
)
{
	int r;
	struct tr31_header_t* header;
	size_t opt_blk_len_total = 0;
	unsigned int enc_block_size;
	void* ptr;

	if (!ctx || !kbpk || !key_block || !key_block_len) {
		return -1;
	}
	if (!ctx->key.data || !ctx->key.length) {
		return -2;
	}

	// ensure space for null-termination
	--key_block_len;

	// validate minimum length
	if (key_block_len < TR31_MIN_KEY_BLOCK_LENGTH) {
		return TR31_ERROR_INVALID_LENGTH;
	}
	memset(key_block, 0, key_block_len);

	// validate key block format version
	// set encryption block size for header padding
	switch (ctx->version) {
		case TR31_VERSION_A:
		case TR31_VERSION_C:
			// supported
			enc_block_size = DES_BLOCK_SIZE;
			break;

		case TR31_VERSION_B:
			// supported
			enc_block_size = DES_BLOCK_SIZE;
			break;

		case TR31_VERSION_D:
			// supported
			enc_block_size = AES_BLOCK_SIZE;
			break;

		case TR31_VERSION_E:
			// supported
			enc_block_size = AES_BLOCK_SIZE;
			break;

		default:
			// unsupported
			return TR31_ERROR_UNSUPPORTED_VERSION;
	}

	// populate key block header
	header = (struct tr31_header_t*)key_block;
	header->version_id = ctx->version;
	memset(header->length, '0', sizeof(header->length)); // update later
	header->key_usage = htons(ctx->key.usage);
	header->algorithm = ctx->key.algorithm;
	header->mode_of_use = ctx->key.mode_of_use;
	header->exportability = ctx->key.exportability;
	memset(header->reserved, '0', sizeof(header->reserved));

	// populate key version field
	r = tr31_key_get_key_version(&ctx->key, header->key_version);
	if (r) {
		// return error value as-is
		return r;
	}

	// populate optional block count
	int_to_dec(ctx->opt_blocks_count, header->opt_blocks_count, sizeof(header->opt_blocks_count));
	ptr = header + 1; // optional blocks, if any, are after the header
	if (ctx->opt_blocks_count && !ctx->opt_blocks) {
		// optional block count is non-zero but optional block data is missing
		return TR31_ERROR_INVALID_NUMBER_OF_OPTIONAL_BLOCKS_FIELD;
	}

	// build optional blocks that involve KCV computation
	for (size_t i = 0; i < ctx->opt_blocks_count; ++i) {
		// if optional block KC is present with no data
		if (ctx->opt_blocks[i].id == TR31_OPT_BLOCK_KC &&
			!ctx->opt_blocks[i].data_length &&
			!ctx->opt_blocks[i].data
		) {
			if (!ctx->key.kcv_len) {
				return TR31_ERROR_KCV_NOT_AVAILABLE;
			}

			// build optional block KC (KCV of wrapped key)
			// see ANSI X9.143:2021, 6.3.6.7
			ctx->opt_blocks[i].data_length = tr31_opt_block_kcv_data_length(ctx->key.kcv_len);
			ctx->opt_blocks[i].data = calloc(1, ctx->opt_blocks[i].data_length);
			r = tr31_opt_block_encode_kcv(
				ctx->key.kcv_algorithm,
				ctx->key.kcv,
				ctx->key.kcv_len,
				ctx->opt_blocks[i].data,
				ctx->opt_blocks[i].data_length
			);
			if (r) {
				// internal error
				return -3;
			}
		}

		// if optional block KP is present with no data
		if (ctx->opt_blocks[i].id == TR31_OPT_BLOCK_KP &&
			!ctx->opt_blocks[i].data_length &&
			!ctx->opt_blocks[i].data
		) {
			if (!kbpk->kcv_len) {
				return TR31_ERROR_KCV_NOT_AVAILABLE;
			}

			// build optional block KP (KCV of KBPK)
			// see ANSI X9.143:2021, 6.3.6.7
			ctx->opt_blocks[i].data_length = tr31_opt_block_kcv_data_length(kbpk->kcv_len);
			ctx->opt_blocks[i].data = calloc(1, ctx->opt_blocks[i].data_length);
			r = tr31_opt_block_encode_kcv(
				kbpk->kcv_algorithm,
				kbpk->kcv,
				kbpk->kcv_len,
				ctx->opt_blocks[i].data,
				ctx->opt_blocks[i].data_length
			);
			if (r) {
				// internal error
				return -4;
			}
		}
	}

	// populate optional blocks
	for (size_t i = 0; i < ctx->opt_blocks_count; ++i) {
		size_t opt_blk_len;
		r = tr31_opt_block_export(
			&ctx->opt_blocks[i],
			(void*)key_block + key_block_len - ptr,
			&opt_blk_len,
			ptr
		);
		if (r) {
			// return error value as-is
			return r;
		}

		// compute total optional block length
		opt_blk_len_total += opt_blk_len;

		// advance current pointer
		ptr += opt_blk_len;
	}

	// ANSI X9.143:2021, 6.3.6 (page 19) indicates that the padding block must
	// result in the total length of all optional blocks being a multiple of
	// the encryption block length.
	// ISO 20038:2017, A.2.1 (page 10) indicates that the total length of all
	// optional blocks must be a multiple of the encryption block size and
	// does not make an exception for format version E.
	// So we'll use the encryption block size which is determined by the TR-31
	// format version.
	if (opt_blk_len_total & (enc_block_size-1)) {
		unsigned int pb_len = 4; // Minimum length of optional block PB

		// compute required padding length
		if ((opt_blk_len_total + pb_len) & (enc_block_size-1)) { // if further padding is required
			pb_len = ((opt_blk_len_total + 4 + enc_block_size) & ~(enc_block_size-1)) - opt_blk_len_total;
		}

		if (ptr + pb_len - (void*)header > key_block_len) {
			// optional block length exceeds total key block length
			return TR31_ERROR_INVALID_LENGTH;
		}

		// populate optional block PB
		r = tr31_opt_block_export_PB(ctx, pb_len, ptr);
		if (r) {
			// return error value as-is
			return r;
		}

		// update optional block count in header
		int_to_dec(ctx->opt_blocks_count + 1, header->opt_blocks_count, sizeof(header->opt_blocks_count));

		// update total optional block length
		opt_blk_len_total += pb_len;

		// advance current pointer
		ptr += pb_len;
	}

	// update header data in context object
	ctx->header_length = ptr - (void*)header;
	ctx->header = (void*)header;

	// determine final key block lengths, including key obfuscation padding
	// this will populate these fields:
	// - ctx->length
	// - ctx->payload_length
	// - ctx->authenticator_length
	r = tr31_compute_final_lengths(ctx);
	if (r) {
		// return error value as-is
		return r;
	}
	if (ctx->length > key_block_len) {
		return TR31_ERROR_INVALID_LENGTH;
	}

	// update key block length in header
	int_to_dec(ctx->length, header->length, sizeof(header->length));

	// free internal buffers that my be populated due to reuse of the context object
	if (ctx->payload) {
		free(ctx->payload);
		ctx->payload = NULL;
	}
	if (ctx->authenticator) {
		free(ctx->authenticator);
		ctx->authenticator = NULL;
	}

	switch (ctx->version) {
		case TR31_VERSION_A:
		case TR31_VERSION_C:
			// only allow TDES key block protection keys
			if (kbpk->algorithm != TR31_KEY_ALGORITHM_TDES) {
				return TR31_ERROR_UNSUPPORTED_KBPK_ALGORITHM;
			}

			// encrypt and sign payload
			// this will populate:
			//   ctx->payload
			//   ctx->authenticator
			r = tr31_tdes_encrypt_sign_variant_binding(ctx, kbpk);
			if (r) {
				// return error value as-is
				return r;
			}
			break;

		case TR31_VERSION_B:
			// only allow TDES key block protection keys
			if (kbpk->algorithm != TR31_KEY_ALGORITHM_TDES) {
				return TR31_ERROR_UNSUPPORTED_KBPK_ALGORITHM;
			}

			// sign and encrypt payload
			// this will populate:
			//   ctx->payload
			//   ctx->authenticator
			r = tr31_tdes_encrypt_sign_derivation_binding(ctx, kbpk);
			if (r) {
				// return error value as-is
				return r;
			}
			break;

		case TR31_VERSION_D:
			// only allow AES key block protection keys
			if (kbpk->algorithm != TR31_KEY_ALGORITHM_AES) {
				return TR31_ERROR_UNSUPPORTED_KBPK_ALGORITHM;
			}

			// sign and encrypt payload
			// this will populate:
			//   ctx->payload
			//   ctx->authenticator
			r = tr31_aes_encrypt_sign_derivation_binding(ctx, kbpk);
			if (r) {
				// return error value as-is
				return r;
			}
			break;

		case TR31_VERSION_E:
			// only allow AES key block protection keys
			if (kbpk->algorithm != TR31_KEY_ALGORITHM_AES) {
				return TR31_ERROR_UNSUPPORTED_KBPK_ALGORITHM;
			}

			// sign and encrypt payload
			// this will populate:
			//   ctx->payload
			//   ctx->authenticator
			r = tr31_aes_encrypt_sign_derivation_binding(ctx, kbpk);
			if (r) {
				// return error value as-is
				return r;
			}
			break;

		default:
			// invalid format version
			return -5;
	}

	// ensure that encrypted payload and authenticator are available
	if (!ctx->payload || !ctx->authenticator) {
		// internal error
		return -6;
	}

	// add payload to key block
	r = bin_to_hex(ctx->payload, ctx->payload_length, ptr, key_block_len);
	if (r) {
		// internal error
		return -7;
	}
	ptr += (ctx->payload_length * 2);

	// add authenticator to key block
	r = bin_to_hex(ctx->authenticator, ctx->authenticator_length, ptr, key_block_len);
	if (r) {
		// internal error
		return -8;
	}

	return 0;
}

static int tr31_opt_block_parse(
	const void* ptr,
	size_t remaining_len,
	size_t* opt_blk_len,
	struct tr31_opt_ctx_t* opt_ctx
)
{
	int r;
	const struct tr31_opt_blk_hdr_t* opt_blk_hdr;
	size_t opt_blk_hdr_len;
	const void* opt_blk_data;

	if (!ptr || !opt_blk_len || !opt_ctx) {
		return -1;
	}
	opt_blk_hdr = ptr;
	*opt_blk_len = 0;

	// ensure that enough bytes remain for minimal optional block
	if (remaining_len < sizeof(struct tr31_opt_blk_hdr_t)) {
		return TR31_ERROR_INVALID_LENGTH;
	}

	// parse optional block id
	opt_ctx->id = ntohs(opt_blk_hdr->id);

	// parse optional block length
	r = hex_to_int(opt_blk_hdr->length, sizeof(opt_blk_hdr->length));
	if (r < 0) {
		// parse error
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_LENGTH;
	}
	if (r) {
		// short optional block length
		*opt_blk_len = r;

		// remember header length for later computations
		opt_blk_hdr_len = sizeof(struct tr31_opt_blk_hdr_t);

	} else {
		// extended optional block length
		const struct tr31_opt_blk_hdr_ext_t* opt_blk_hdr_ext = ptr;
		size_t opt_blk_len_byte_count;

		// parse extended length byte count
		if (sizeof(struct tr31_opt_blk_hdr_ext_t) > remaining_len) {
			// optional block parsing exceeds remaining key block length
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_LENGTH;
		}
		r = hex_to_int(opt_blk_hdr_ext->ext_length_byte_count, sizeof(opt_blk_hdr_ext->ext_length_byte_count));
		if (r < 0) {
			// parse error
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_LENGTH;
		}
		opt_blk_len_byte_count = r;

		// parse extended length field
		if (sizeof(struct tr31_opt_blk_hdr_ext_t) + opt_blk_len_byte_count > remaining_len) {
			// optional block parsing exceeds remaining key block length
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_LENGTH;
		}
		r = hex_to_int(opt_blk_hdr_ext->ext_length, opt_blk_len_byte_count);
		if (r < 0) {
			// parse error
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_LENGTH;
		}
		*opt_blk_len = r;

		// remember header length for later computations
		opt_blk_hdr_len = sizeof(struct tr31_opt_blk_hdr_ext_t) + opt_blk_len_byte_count;
	}

	// ensure that optional block length is valid
	if (*opt_blk_len < opt_blk_hdr_len) {
		// optional block length is less than optional header length
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_LENGTH;
	}
	if (*opt_blk_len > remaining_len) {
		// optional block length exceeds remaining key block length
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_LENGTH;
	}
	opt_blk_data = ptr + opt_blk_hdr_len;

	switch (opt_ctx->id) {
		// optional blocks to be validated as hex (format H)
		case TR31_OPT_BLOCK_AL:
		case TR31_OPT_BLOCK_BI:
		case TR31_OPT_BLOCK_HM:
		case TR31_OPT_BLOCK_IK:
		case TR31_OPT_BLOCK_KC:
		case TR31_OPT_BLOCK_KP:
		case TR31_OPT_BLOCK_KS:
		case TR31_OPT_BLOCK_PK:
			opt_ctx->data_length = (*opt_blk_len - opt_blk_hdr_len);
			r = tr31_validate_format_h(opt_blk_data, opt_ctx->data_length);
			if (r) {
				return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
			}
			opt_ctx->data = malloc(opt_ctx->data_length);
			memcpy(opt_ctx->data, opt_blk_data, opt_ctx->data_length);
			return 0;

		// optional blocks to be validated as alphanumeric (format AN)
		case TR31_OPT_BLOCK_DA:
			opt_ctx->data_length = (*opt_blk_len - opt_blk_hdr_len);
			r = tr31_validate_format_an(opt_blk_data, opt_ctx->data_length);
			if (r) {
				return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
			}
			opt_ctx->data = malloc(opt_ctx->data_length);
			memcpy(opt_ctx->data, opt_blk_data, opt_ctx->data_length);
			return 0;

		// optional blocks to be validated as printable ASCII (format PA)
		case TR31_OPT_BLOCK_LB:
		case TR31_OPT_BLOCK_PB:
			opt_ctx->data_length = (*opt_blk_len - opt_blk_hdr_len);
			r = tr31_validate_format_pa(opt_blk_data, opt_ctx->data_length);
			if (r) {
				return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
			}
			opt_ctx->data = malloc(opt_ctx->data_length);
			memcpy(opt_ctx->data, opt_blk_data, opt_ctx->data_length);
			return 0;

		// optional blocks to be validated as ISO 8601
		case TR31_OPT_BLOCK_TC:
		case TR31_OPT_BLOCK_TS:
			opt_ctx->data_length = (*opt_blk_len - opt_blk_hdr_len);
			r = tr31_opt_block_validate_iso8601(opt_blk_data, opt_ctx->data_length);
			if (r) {
				return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
			}
			opt_ctx->data = malloc(opt_ctx->data_length);
			memcpy(opt_ctx->data, opt_blk_data, opt_ctx->data_length);
			return 0;

		// copy all other optional blocks, including proprietary ones, verbatim
		default:
			opt_ctx->data_length = (*opt_blk_len - opt_blk_hdr_len);
			opt_ctx->data = malloc(opt_ctx->data_length);
			memcpy(opt_ctx->data, opt_blk_data, opt_ctx->data_length);
			return 0;
	}
}

static int tr31_opt_block_validate_iso8601(const char* str, size_t str_len)
{
	if (!str) {
		return -1;
	}

	// NOTE: this function only performs basic format checks and is not
	// intended to perform strict ISO 8601 format validation nor determine the
	// correctness of the date or time

	// validate ISO 8601 string length
	// see ANSI X9.143:2021, 6.3.6.13, table 21
	// see ANSI X9.143:2021, 6.3.6.14, table 22
	if (str_len != 0x13 - 4 && // no delimiters, ss precision
		str_len != 0x15 - 4 && // no delimiters, ssss precision
		str_len != 0x18 - 4 && // delimiters, ss precision
		str_len != 0x1B - 4 // delimiters, ss.ss precision
	) {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	// validate ISO 8601 designator (must be UTC)
	// see ANSI X9.143:2021, 6.3.6.13
	// see ANSI X9.143:2021, 6.3.6.14
	if (str[str_len-1] != 'Z') {
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	// validate ISO 8601 delimiters (YYYY-MM-DDThh:mm:ss[.ss])
	if (str_len == 0x18 || str_len == 0x1B) {
		if (str[4] != '-' ||
			str[7] != '-' ||
			str[10] != 'T' ||
			str[13] != ':' ||
			str[16] != ':'
		) {
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
		}
	}
	if (str_len == 0x1B) {
		if (str[19] != '.') {
			return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
		}
	}

	return 0;
}

static int tr31_opt_block_export(
	const struct tr31_opt_ctx_t* opt_ctx,
	size_t remaining_len,
	size_t* opt_blk_len,
	void* ptr
)
{
	int r;
	struct tr31_opt_blk_hdr_t* opt_blk_hdr;
	size_t encoded_data_length;
	bool encode_hex = false;
	const size_t opt_blk_len_byte_count = 4; // must be 4 according to ANSI X9.143:2021, 6.2, table 1
	size_t opt_blk_hdr_len;
	void* opt_blk_data;

	if (!opt_ctx || !opt_blk_len || !ptr) {
		return -1;
	}
	opt_blk_hdr = ptr;
	*opt_blk_len = 0;

	if (remaining_len < sizeof(struct tr31_opt_blk_hdr_t)) {
		// minimal optional block lengths exceeded remaining key block length
		return TR31_ERROR_INVALID_LENGTH;
	}

	if (opt_ctx->data_length && !opt_ctx->data) {
		// optional block payload length is non-zero but optional block data is missing
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA;
	}

	// populate optional block id
	opt_blk_hdr->id = htons(opt_ctx->id);

	// determine optional block data length according to encoding
	switch (opt_ctx->id) {
		// optional blocks to be copied verbatim
		default:
			encoded_data_length = opt_ctx->data_length;
			break;
	}

	// populate optional block length
	if (encoded_data_length + sizeof(struct tr31_opt_blk_hdr_t) < 256) {
		// short optional block length
		*opt_blk_len = encoded_data_length + sizeof(struct tr31_opt_blk_hdr_t);
		if (*opt_blk_len > remaining_len) {
			// optional block length exceeds remaining key block length
			return TR31_ERROR_INVALID_LENGTH;
		}
		int_to_hex(*opt_blk_len, opt_blk_hdr->length, sizeof(opt_blk_hdr->length));

		// remember header length for later computations
		opt_blk_hdr_len = sizeof(struct tr31_opt_blk_hdr_t);

	} else if (encoded_data_length + sizeof(struct tr31_opt_blk_hdr_ext_t) + opt_blk_len_byte_count < 65536) {
		// extended optional block length
		struct tr31_opt_blk_hdr_ext_t* opt_blk_hdr_ext = ptr;
		*opt_blk_len = encoded_data_length + sizeof(struct tr31_opt_blk_hdr_ext_t) + opt_blk_len_byte_count;
		if (*opt_blk_len > remaining_len) {
			// optional block length exceeds remaining key block length
			return TR31_ERROR_INVALID_LENGTH;
		}

		// populate extended optional block length
		memset(opt_blk_hdr_ext->reserved, 0x30, sizeof(opt_blk_hdr_ext->reserved));
		int_to_hex(opt_blk_len_byte_count, opt_blk_hdr_ext->ext_length_byte_count, sizeof(opt_blk_hdr_ext->ext_length_byte_count));
		int_to_hex(*opt_blk_len, opt_blk_hdr_ext->ext_length, opt_blk_len_byte_count);

		// remember header length for later computations
		opt_blk_hdr_len = sizeof(struct tr31_opt_blk_hdr_ext_t) + opt_blk_len_byte_count;

	} else {
		// unsupported optional block length
		return TR31_ERROR_INVALID_OPTIONAL_BLOCK_LENGTH;
	}
	opt_blk_data = ptr + opt_blk_hdr_len;

	// populate optional block data
	if (encode_hex) {
		// encode as hex
		r = bin_to_hex(
			opt_ctx->data,
			opt_ctx->data_length,
			opt_blk_data,
			opt_ctx->data_length * 2
		);
		if (r) {
			return -2;
		}
	} else {
		// copy verbatim
		memcpy(opt_blk_data, opt_ctx->data, opt_ctx->data_length);
	}

	return 0;
}

static int tr31_opt_block_export_PB(
	const struct tr31_ctx_t* ctx,
	size_t pb_len,
	struct tr31_opt_blk_t* opt_blk
)
{
	opt_blk->id = htons(TR31_OPT_BLOCK_PB);
	int_to_hex(pb_len, opt_blk->length, sizeof(opt_blk->length));

	if ((ctx->export_flags & TR31_EXPORT_ZERO_OPT_BLOCK_PB) == 0) {
		// populate with random data and then transpose to the required range
		crypto_rand(opt_blk->data, pb_len - 4);
	} else {
		// populate with zeros instead of random data
		memset(opt_blk->data, 0, pb_len - 4);
	}

	for (size_t i = 0; i < pb_len - 4; ++i) {
		// although optional block PB may contain printable ASCII characters in
		// the range 0x20 to 0x7E, characters outside the ranges of '0'-'9',
		// 'A'-'Z' and 'a'-'z' are problematic when using HSM protocols that
		// may use other printable ASCII characters as delimiters

		// use unsigned integers for sanity but cast to uint8_t to fix negative
		// char values without setting high order bits due to 2s complement
		unsigned int tmp = (uint8_t)opt_blk->data[i];

		// clamp range to [0 - 61] for 62 possible characters
		tmp = (tmp * 61) / 0xFF;

		// split range into ranges of '0'-'9', 'A'-'Z' and 'a'-'z'
		if (tmp < 10) {
			opt_blk->data[i] = tmp + '0'; // '0'-'9'
		} else if (tmp < 36) {
			opt_blk->data[i] = tmp - 10 + 'A'; // 'A'-'Z'
		} else if (tmp < 62) {
			opt_blk->data[i] = tmp - 36 + 'a'; // 'a'-'z'
		} else {
			// This should never happen
			return -1;
		}
	}

	return 0;
}

static int tr31_compute_final_lengths(struct tr31_ctx_t* ctx)
{
	size_t padded_key_length;

	// validate key length by algorithm
	// this ensures that key length cannot exceed padded key length
	switch (ctx->key.algorithm) {
		case TR31_KEY_ALGORITHM_TDES:
			if (ctx->key.length > 24) {
				// invalid TDES key length
				return TR31_ERROR_INVALID_KEY_LENGTH;
			}
			break;

		case TR31_KEY_ALGORITHM_AES:
			if (ctx->key.length > 32) {
				// invalid AES key length
				return TR31_ERROR_INVALID_KEY_LENGTH;
			}
			break;
	}

	// use key length as-is by default
	padded_key_length = ctx->key.length;

	if ((ctx->export_flags & TR31_EXPORT_NO_KEY_LENGTH_OBFUSCATION) == 0) {
		// apply key length obfuscation
		// see ANSI X9.143:2021, 5 and 6.1
		switch (ctx->key.algorithm) {
			case TR31_KEY_ALGORITHM_TDES:
				// use maximum TDES length
				padded_key_length = 24;
				break;

			case TR31_KEY_ALGORITHM_AES:
				// use maximum AES length
				padded_key_length = 32;
				break;
		}
	}

	switch (ctx->version) {
		case TR31_VERSION_A:
		case TR31_VERSION_C:
			ctx->payload_length = DES_CIPHERTEXT_LENGTH(sizeof(struct tr31_payload_t) + padded_key_length);
			ctx->authenticator_length = 4; // 4 bytes; 8 ASCII hex digits
			break;

		case TR31_VERSION_B:
			ctx->payload_length = DES_CIPHERTEXT_LENGTH(sizeof(struct tr31_payload_t) + padded_key_length);
			ctx->authenticator_length = 8; // 8 bytes; 16 ASCII hex digits
			break;

		case TR31_VERSION_D:
			ctx->payload_length = AES_CIPHERTEXT_LENGTH(sizeof(struct tr31_payload_t) + padded_key_length);
			ctx->authenticator_length = 16; // 16 bytes; 32 ASCII hex digits
			break;

		case TR31_VERSION_E:
			ctx->payload_length = sizeof(struct tr31_payload_t) + padded_key_length; // no additional padding required
			ctx->authenticator_length = 16; // 16 bytes; 32 ASCII hex digits
			break;

		default:
			// unsupported
			return TR31_ERROR_UNSUPPORTED_VERSION;
	}

	ctx->length =
		+ ctx->header_length
		+ (ctx->payload_length * 2)
		+ (ctx->authenticator_length * 2);

	return 0;
}

static int tr31_tdes_decrypt_verify_variant_binding(struct tr31_ctx_t* ctx, const struct tr31_key_t* kbpk)
{
	int r;
	uint8_t kbek[TDES3_KEY_SIZE];
	uint8_t kbak[TDES3_KEY_SIZE];
	size_t key_length;

	// buffer for decryption
	uint8_t decrypted_payload_buf[ctx->payload_length];
	struct tr31_payload_t* decrypted_payload = (struct tr31_payload_t*)decrypted_payload_buf;

	// buffer for MAC verification
	uint8_t mac_input[ctx->header_length + ctx->payload_length];
	memcpy(mac_input, ctx->header, ctx->header_length);
	memcpy(mac_input + ctx->header_length, ctx->payload, ctx->payload_length);

	// output key block encryption key variant and key block authentication key variant
	r = tr31_tdes_kbpk_variant(kbpk->data, kbpk->length, kbek, kbak);
	if (r) {
		// return error value as-is
		goto error;
	}

	// verify authenticator
	r = tr31_tdes_verify_cbcmac(kbak, kbpk->length, mac_input, sizeof(mac_input), ctx->authenticator, ctx->authenticator_length);
	if (r) {
		r = TR31_ERROR_KEY_BLOCK_VERIFICATION_FAILED;
		goto error;
	}

	// decrypt key payload; note that the TR-31 header is used as the IV
	r = crypto_tdes_decrypt(kbek, kbpk->length, ctx->header, ctx->payload, ctx->payload_length, decrypted_payload);
	if (r) {
		// return error value as-is
		goto error;
	}

	// validate payload length field
	key_length = ntohs(decrypted_payload->length) / 8; // payload length is big endian and in bits, not bytes
	if (key_length > ctx->payload_length - 2) {
		// invalid key length relative to encrypted payload length
		r = TR31_ERROR_INVALID_KEY_LENGTH;
		goto error;
	}

	// extract key data
	r = tr31_key_set_data(&ctx->key, decrypted_payload->data, key_length);
	if (r) {
		// return error value as-is
		goto error;
	}

	// success
	r = 0;
	goto exit;

error:
exit:
	// cleanse sensitive buffers
	crypto_cleanse(kbek, sizeof(kbek));
	crypto_cleanse(kbak, sizeof(kbak));
	crypto_cleanse(decrypted_payload_buf, sizeof(decrypted_payload_buf));
	crypto_cleanse(mac_input, sizeof(mac_input));

	return r;
}

static int tr31_tdes_encrypt_sign_variant_binding(struct tr31_ctx_t* ctx, const struct tr31_key_t* kbpk)
{
	int r;
	uint8_t kbek[TDES3_KEY_SIZE];
	uint8_t kbak[TDES3_KEY_SIZE];

	// add payload data to context object
	ctx->payload = calloc(1, ctx->payload_length);

	// add authenticator to context object
	ctx->authenticator = calloc(1, ctx->authenticator_length);

	// buffer for encrypted
	uint8_t decrypted_payload_buf[ctx->payload_length];
	struct tr31_payload_t* decrypted_payload = (struct tr31_payload_t*)decrypted_payload_buf;

	// buffer for MAC generation
	uint8_t mac_input[ctx->header_length + ctx->payload_length];

	// populate payload key
	decrypted_payload->length = htons(ctx->key.length * 8); // payload length is big endian and in bits, not bytes
	memcpy(decrypted_payload->data, ctx->key.data, ctx->key.length);
	crypto_rand(
		decrypted_payload->data + ctx->key.length,
		ctx->payload_length - sizeof(struct tr31_payload_t) - ctx->key.length
	);

	// output key block encryption key variant and key block authentication key variant
	r = tr31_tdes_kbpk_variant(kbpk->data, kbpk->length, kbek, kbak);
	if (r) {
		// return error value as-is
		goto error;
	}

	// encrypt key payload; note that the TR-31 header is used as the IV
	r = crypto_tdes_encrypt(kbek, kbpk->length, ctx->header, decrypted_payload, ctx->payload_length, ctx->payload);
	if (r) {
		// return error value as-is
		goto error;
	}

	// generate authenticator
	uint8_t mac[DES_CBCMAC_SIZE];
	memcpy(mac_input, ctx->header, ctx->header_length);
	memcpy(mac_input + ctx->header_length, ctx->payload, ctx->payload_length);
	r = crypto_tdes_cbcmac(kbak, kbpk->length, mac_input, sizeof(mac_input), mac);
	if (r) {
		// return error value as-is
		goto error;
	}
	memcpy(ctx->authenticator, mac, ctx->authenticator_length);

	// success
	r = 0;
	goto exit;

error:
exit:
	// cleanse sensitive buffers
	crypto_cleanse(kbek, sizeof(kbek));
	crypto_cleanse(kbak, sizeof(kbak));
	crypto_cleanse(decrypted_payload_buf, sizeof(decrypted_payload_buf));
	crypto_cleanse(mac_input, sizeof(mac_input));
	crypto_cleanse(mac, sizeof(mac));

	return r;
}

static int tr31_tdes_decrypt_verify_derivation_binding(struct tr31_ctx_t* ctx, const struct tr31_key_t* kbpk)
{
	int r;
	uint8_t kbek[TDES3_KEY_SIZE];
	uint8_t kbak[TDES3_KEY_SIZE];
	size_t key_length;

	// buffer for decryption and CMAC verification
	uint8_t decrypted_key_block[ctx->header_length + ctx->payload_length];
	memcpy(decrypted_key_block, ctx->header, ctx->header_length);
	struct tr31_payload_t* decrypted_payload = (struct tr31_payload_t*)(decrypted_key_block + ctx->header_length);

	// derive key block encryption key and key block authentication key from key block protection key
	r = tr31_tdes_kbpk_derive(kbpk->data, kbpk->length, kbek, kbak);
	if (r) {
		// return error value as-is
		goto error;
	}

	// decrypt key payload; note that the authenticator is used as the IV
	r = crypto_tdes_decrypt(kbek, kbpk->length, ctx->authenticator, ctx->payload, ctx->payload_length, decrypted_payload);
	if (r) {
		// return error value as-is
		goto error;
	}

	// extract payload length field
	key_length = ntohs(decrypted_payload->length) / 8; // payload length is big endian and in bits, not bytes
	if (key_length > ctx->payload_length - 2) {
		// invalid key length relative to encrypted payload length
		r = TR31_ERROR_INVALID_KEY_LENGTH;
		goto error;
	}

	// verify authenticator
	r = tr31_tdes_verify_cmac(kbak, kbpk->length, decrypted_key_block, sizeof(decrypted_key_block), ctx->authenticator, ctx->authenticator_length);
	if (r) {
		r = TR31_ERROR_KEY_BLOCK_VERIFICATION_FAILED;
		goto error;
	}

	// extract key data
	r = tr31_key_set_data(&ctx->key, decrypted_payload->data, key_length);
	if (r) {
		// return error value as-is
		goto error;
	}

	// success
	r = 0;
	goto exit;

error:
exit:
	// cleanse sensitive buffers
	crypto_cleanse(kbek, sizeof(kbek));
	crypto_cleanse(kbak, sizeof(kbak));
	crypto_cleanse(decrypted_key_block, sizeof(decrypted_key_block));

	return r;
}

static int tr31_tdes_encrypt_sign_derivation_binding(struct tr31_ctx_t* ctx, const struct tr31_key_t* kbpk)
{
	int r;
	uint8_t kbek[TDES3_KEY_SIZE];
	uint8_t kbak[TDES3_KEY_SIZE];

	// add payload data to context object
	ctx->payload = calloc(1, ctx->payload_length);

	// add authenticator to context object
	ctx->authenticator = calloc(1, ctx->authenticator_length);

	// buffer for CMAC generation and encryption
	uint8_t decrypted_key_block[ctx->header_length + ctx->payload_length];
	memcpy(decrypted_key_block, ctx->header, ctx->header_length);
	struct tr31_payload_t* decrypted_payload = (struct tr31_payload_t*)(decrypted_key_block + ctx->header_length);

	// populate payload key
	decrypted_payload->length = htons(ctx->key.length * 8); // payload length is big endian and in bits, not bytes
	memcpy(decrypted_payload->data, ctx->key.data, ctx->key.length);
	crypto_rand(
		decrypted_payload->data + ctx->key.length,
		ctx->payload_length - sizeof(struct tr31_payload_t) - ctx->key.length
	);

	// derive key block encryption key and key block authentication key from key block protection key
	r = tr31_tdes_kbpk_derive(kbpk->data, kbpk->length, kbek, kbak);
	if (r) {
		// return error value as-is
		goto error;
	}

	// generate authenticator
	uint8_t cmac[DES_CMAC_SIZE];
	r = crypto_tdes_cmac(kbak, kbpk->length, decrypted_key_block, sizeof(decrypted_key_block), cmac);
	if (r) {
		// return error value as-is
		goto error;
	}
	memcpy(ctx->authenticator, cmac, ctx->authenticator_length);

	// encrypt key payload; note that the authenticator is used as the IV
	r = crypto_tdes_encrypt(kbek, kbpk->length, ctx->authenticator, decrypted_payload, ctx->payload_length, ctx->payload);
	if (r) {
		// return error value as-is
		goto error;
	}

	// success
	r = 0;
	goto exit;

error:
exit:
	// cleanse sensitive buffers
	crypto_cleanse(kbek, sizeof(kbek));
	crypto_cleanse(kbak, sizeof(kbak));
	crypto_cleanse(decrypted_key_block, sizeof(decrypted_key_block));
	crypto_cleanse(cmac, sizeof(cmac));

	return r;
}

static int tr31_aes_decrypt_verify_derivation_binding(struct tr31_ctx_t* ctx, const struct tr31_key_t* kbpk)
{
	int r;
	uint8_t kbek[AES256_KEY_SIZE];
	uint8_t kbak[AES256_KEY_SIZE];
	size_t key_length;

	// buffer for decryption and CMAC verification
	uint8_t decrypted_key_block[ctx->header_length + ctx->payload_length];
	memcpy(decrypted_key_block, ctx->header, ctx->header_length);
	struct tr31_payload_t* decrypted_payload = (struct tr31_payload_t*)(decrypted_key_block + ctx->header_length);

	if (ctx->version == TR31_VERSION_D) {
		// derive key block encryption key and key block authentication key from key block protection key
		// format version D uses CBC block mode
		r = tr31_aes_kbpk_derive(kbpk->data, kbpk->length, TR31_AES_MODE_CBC, kbek, kbak);
		if (r) {
			// return error value as-is
			goto error;
		}

		// decrypt key payload; note that the authenticator is used as the IV
		r = crypto_aes_decrypt(kbek, kbpk->length, ctx->authenticator, ctx->payload, ctx->payload_length, decrypted_payload);
		if (r) {
			// return error value as-is
			goto error;
		}

	} else if (ctx->version == TR31_VERSION_E) {
		// derive key block encryption key and key block authentication key from key block protection key
		// format version E uses CTR block mode
		r = tr31_aes_kbpk_derive(kbpk->data, kbpk->length, TR31_AES_MODE_CTR, kbek, kbak);
		if (r) {
			// return error value as-is
			goto error;
		}

		// decrypt key payload; note that the authenticator is used as the IV/nonce
		r = crypto_aes_decrypt_ctr(kbek, kbpk->length, ctx->authenticator, ctx->payload, ctx->payload_length, decrypted_payload);
		if (r) {
			// return error value as-is
			goto error;
		}

	} else {
		// invalid format version
		return -1;
	}

	// extract payload length field
	key_length = ntohs(decrypted_payload->length) / 8; // payload length is big endian and in bits, not bytes
	if (key_length > ctx->payload_length - 2) {
		// invalid key length relative to encrypted payload length
		r = TR31_ERROR_INVALID_KEY_LENGTH;
		goto error;
	}

	// verify authenticator
	r = tr31_aes_verify_cmac(kbak, kbpk->length, decrypted_key_block, sizeof(decrypted_key_block), ctx->authenticator, ctx->authenticator_length);
	if (r) {
		r = TR31_ERROR_KEY_BLOCK_VERIFICATION_FAILED;
		goto error;
	}

	// extract key data
	r = tr31_key_set_data(&ctx->key, decrypted_payload->data, key_length);
	if (r) {
		// return error value as-is
		goto error;
	}

	// success
	r = 0;
	goto exit;

error:
exit:
	// cleanse sensitive buffers
	crypto_cleanse(kbek, sizeof(kbek));
	crypto_cleanse(kbak, sizeof(kbak));
	crypto_cleanse(decrypted_key_block, sizeof(decrypted_key_block));

	return r;
}

static int tr31_aes_encrypt_sign_derivation_binding(struct tr31_ctx_t* ctx, const struct tr31_key_t* kbpk)
{
	int r;
	uint8_t kbek[AES256_KEY_SIZE];
	uint8_t kbak[AES256_KEY_SIZE];
	uint8_t cmac[AES_CMAC_SIZE];

	// add payload data to context object
	ctx->payload = calloc(1, ctx->payload_length);

	// add authenticator to context object
	ctx->authenticator = calloc(1, ctx->authenticator_length);

	// buffer for CMAC generation and encryption
	uint8_t decrypted_key_block[ctx->header_length + ctx->payload_length];
	memcpy(decrypted_key_block, ctx->header, ctx->header_length);
	struct tr31_payload_t* decrypted_payload = (struct tr31_payload_t*)(decrypted_key_block + ctx->header_length);

	// populate payload key
	decrypted_payload->length = htons(ctx->key.length * 8); // payload length is big endian and in bits, not bytes
	memcpy(decrypted_payload->data, ctx->key.data, ctx->key.length);
	crypto_rand(
		decrypted_payload->data + ctx->key.length,
		ctx->payload_length - sizeof(struct tr31_payload_t) - ctx->key.length
	);

	if (ctx->version == TR31_VERSION_D) {
		// derive key block encryption key and key block authentication key from key block protection key
		// format version D uses CBC block mode
		r = tr31_aes_kbpk_derive(kbpk->data, kbpk->length, TR31_AES_MODE_CBC, kbek, kbak);
		if (r) {
			// return error value as-is
			goto error;
		}

		// generate authenticator
		r = crypto_aes_cmac(kbak, kbpk->length, decrypted_key_block, sizeof(decrypted_key_block), cmac);
		if (r) {
			// return error value as-is
			goto error;
		}
		memcpy(ctx->authenticator, cmac, ctx->authenticator_length);

		// encrypt key payload; note that the authenticator is used as the IV
		r = crypto_aes_encrypt(kbek, kbpk->length, ctx->authenticator, decrypted_payload, ctx->payload_length, ctx->payload);
		if (r) {
			// return error value as-is
			goto error;
		}

	} else if (ctx->version == TR31_VERSION_E) {
		// derive key block encryption key and key block authentication key from key block protection key
		// format version E uses CTR block mode
		r = tr31_aes_kbpk_derive(kbpk->data, kbpk->length, TR31_AES_MODE_CTR, kbek, kbak);
		if (r) {
			// return error value as-is
			goto error;
		}

		// generate authenticator
		r = crypto_aes_cmac(kbak, kbpk->length, decrypted_key_block, sizeof(decrypted_key_block), cmac);
		if (r) {
			// return error value as-is
			goto error;
		}
		memcpy(ctx->authenticator, cmac, ctx->authenticator_length);

		// encrypt key payload; note that the authenticator is used as the IV/nonce
		r = crypto_aes_encrypt_ctr(kbek, kbpk->length, ctx->authenticator, decrypted_payload, ctx->payload_length, ctx->payload);
		if (r) {
			// return error value as-is
			goto error;
		}

	} else {
		// invalid format version
		return -1;
	}

	// success
	r = 0;
	goto exit;

error:
exit:
	// cleanse sensitive buffers
	crypto_cleanse(kbek, sizeof(kbek));
	crypto_cleanse(kbak, sizeof(kbak));
	crypto_cleanse(decrypted_key_block, sizeof(decrypted_key_block));
	crypto_cleanse(cmac, sizeof(cmac));

	return r;
}

void tr31_release(struct tr31_ctx_t* ctx)
{
	if (!ctx) {
		return;
	}

	tr31_key_release(&ctx->key);

	if (ctx->opt_blocks) {
		for (size_t i = 0; i < ctx->opt_blocks_count; ++i) {
			if (ctx->opt_blocks[i].data) {
				free(ctx->opt_blocks[i].data);
			}
			ctx->opt_blocks[i].data = NULL;
		}

		free(ctx->opt_blocks);
		ctx->opt_blocks = NULL;
	}

	if (ctx->payload) {
		free(ctx->payload);
		ctx->payload = NULL;
	}
	if (ctx->authenticator) {
		free(ctx->authenticator);
		ctx->authenticator = NULL;
	}
}

const char* tr31_get_error_string(enum tr31_error_t error)
{
	if (error < 0) {
		return "Internal error";
	}

	switch (error) {
		case TR31_ERROR_INVALID_LENGTH: return "Invalid key block length";
		case TR31_ERROR_UNSUPPORTED_VERSION: return "Unsupported key block format version";
		case TR31_ERROR_INVALID_LENGTH_FIELD: return "Invalid key block length field";
		case TR31_ERROR_UNSUPPORTED_KEY_USAGE: return "Unsupported key usage";
		case TR31_ERROR_UNSUPPORTED_ALGORITHM: return "Unsupported key algorithm";
		case TR31_ERROR_UNSUPPORTED_MODE_OF_USE: return "Unsupported key mode of use";
		case TR31_ERROR_INVALID_KEY_VERSION_FIELD: return "Invalid key version field";
		case TR31_ERROR_UNSUPPORTED_EXPORTABILITY: return "Unsupported key exportability";
		case TR31_ERROR_INVALID_NUMBER_OF_OPTIONAL_BLOCKS_FIELD: return "Invalid number of optional blocks field";
		case TR31_ERROR_INVALID_OPTIONAL_BLOCK_LENGTH: return "Invalid optional block length";
		case TR31_ERROR_INVALID_OPTIONAL_BLOCK_DATA: return "Invalid optional block data";
		case TR31_ERROR_INVALID_PAYLOAD_FIELD: return "Invalid payload data field";
		case TR31_ERROR_INVALID_AUTHENTICATOR_FIELD: return "Invalid authenticator data field";
		case TR31_ERROR_UNSUPPORTED_KBPK_ALGORITHM: return "Unsupported key block protection key algorithm";
		case TR31_ERROR_UNSUPPORTED_KBPK_LENGTH: return "Unsupported key block protection key length";
		case TR31_ERROR_INVALID_KEY_LENGTH: return "Invalid key length";
		case TR31_ERROR_KEY_BLOCK_VERIFICATION_FAILED: return "Key block verification failed";
		case TR31_ERROR_KCV_NOT_AVAILABLE: return "Key check value not available";
	}

	return "Unknown error";
}

const char* tr31_get_key_usage_ascii(unsigned int usage, char* ascii, size_t ascii_len)
{
	union {
		uint16_t value;
		char bytes[2];
	} usage_ascii;

	usage_ascii.value = htons(usage);

	if (ascii_len < 3) {
		return NULL;
	}
	for (size_t i = 0; i < sizeof(usage_ascii.bytes); ++i) {
		if (tr31_validate_format_an(usage_ascii.bytes, sizeof(usage_ascii.bytes)) == 0) {
			ascii[i] = usage_ascii.bytes[i];
		} else {
			ascii[i] = '?';
		}
	}
	ascii[2] = 0;

	return ascii;
}

const char* tr31_get_key_usage_string(unsigned int usage)
{
	// see ANSI X9.143:2021, 6.3.1, table 2
	switch (usage) {
		case TR31_KEY_USAGE_BDK:                return "Base Derivation Key (BDK)";
		case TR31_KEY_USAGE_DUKPT_IK:           return "Initial DUKPT Key (IK/IPEK)";
		case TR31_KEY_USAGE_BKV:                return "Base Key Variant Key";
		case TR31_KEY_USAGE_KDK:                return "Key Derivation Key";
		case TR31_KEY_USAGE_CVK:                return "Card Verification Key (CVK)";
		case TR31_KEY_USAGE_DATA:               return "Symmetric Key for Data Encryption";
		case TR31_KEY_USAGE_ASYMMETRIC_DATA:    return "Asymmetric Key for Data Encryption";
		case TR31_KEY_USAGE_DATA_DEC_TABLE:     return "Data Encryption Key for Decimalization Table";
		case TR31_KEY_USAGE_DATA_SENSITIVE:     return "Data Encryption Key for Sensitive Data";
		case TR31_KEY_USAGE_EMV_MKAC:           return "EMV/Chip Issuer Master Key: Application Cryptograms (MKAC)";
		case TR31_KEY_USAGE_EMV_MKSMC:          return "EMV/Chip Issuer Master Key: Secure Messaging for Confidentiality (MKSMC)";
		case TR31_KEY_USAGE_EMV_MKSMI:          return "EMV/Chip Issuer Master Key: Secure Messaging for Integrity (MKSMI)";
		case TR31_KEY_USAGE_EMV_MKDAC:          return "EMV/Chip Issuer Master Key: Data Authentication Code (MKDAC)";
		case TR31_KEY_USAGE_EMV_MKDN:           return "EMV/Chip Issuer Master Key: Dynamic Numbers (MKDN)";
		case TR31_KEY_USAGE_EMV_CP:             return "EMV/Chip Issuer Master Key: Card Personalization (CP)";
		case TR31_KEY_USAGE_EMV_OTHER:          return "EMV/Chip Issuer Master Key: Other";
		case TR31_KEY_USAGE_EMV_AKP_PIN:        return "EMV/Chip Asymmetric Key Pair for PIN Encryption";
		case TR31_KEY_USAGE_IV:                 return "Initialization Vector (IV)";
		case TR31_KEY_USAGE_KEK:                return "Key Encryption or Wrapping Key (KEK)";
		case TR31_KEY_USAGE_TR31_KBPK:          return "TR-31 Key Block Protection Key (KBPK)";
		case TR31_KEY_USAGE_TR34_APK_KRD:       return "TR-34 Asymmetric Key Pair for Key Receiving Device";
		case TR31_KEY_USAGE_APK:                return "Asymmetric Key Pair for Key Wrapping or Key Agreement";
		case TR31_KEY_USAGE_ISO20038_KBPK:      return "ISO 20038 Key Block Protection Key (KBPK)";
		case TR31_KEY_USAGE_ISO16609_MAC_1:     return "ISO 16609 MAC algorithm 1 (using TDES)";
		case TR31_KEY_USAGE_ISO9797_1_MAC_1:    return "ISO 9797-1 MAC Algorithm 1 (CBC-MAC)";
		case TR31_KEY_USAGE_ISO9797_1_MAC_2:    return "ISO 9797-1 MAC Algorithm 2";
		case TR31_KEY_USAGE_ISO9797_1_MAC_3:    return "ISO 9797-1 MAC Algorithm 3 (Retail MAC)";
		case TR31_KEY_USAGE_ISO9797_1_MAC_4:    return "ISO 9797-1 MAC Algorithm 4";
		case TR31_KEY_USAGE_ISO9797_1_MAC_5:    return "ISO 9797-1:1999 MAC Algorithm 5 (legacy)";
		case TR31_KEY_USAGE_ISO9797_1_CMAC:     return "ISO 9797-1:2011 MAC Algorithm 5 (CMAC)";
		case TR31_KEY_USAGE_HMAC:               return "HMAC Key";
		case TR31_KEY_USAGE_ISO9797_1_MAC_6:    return "ISO 9797-1 MAC Algorithm 6";
		case TR31_KEY_USAGE_PEK:                return "PIN Encryption Key";
		case TR31_KEY_USAGE_PGK:                return "PIN Generation Key";
		case TR31_KEY_USAGE_AKP_SIG:            return "Asymmetric Key Pair for Digital Signature";
		case TR31_KEY_USAGE_AKP_CA:             return "Asymmetric Key Pair for CA use";
		case TR31_KEY_USAGE_AKP_OTHER:          return "Asymmetric Key Pair for non-X9.24 use";
		case TR31_KEY_USAGE_PVK:                return "PIN Verification Key (Other)";
		case TR31_KEY_USAGE_PVK_IBM3624:        return "PIN Verification Key (IBM 3624)";
		case TR31_KEY_USAGE_PVK_VISA_PVV:       return "PIN Verification Key (VISA PVV)";
		case TR31_KEY_USAGE_PVK_X9_132_ALG_1:   return "PIN Verification Key (ANSI X9.132 algorithm 1)";
		case TR31_KEY_USAGE_PVK_X9_132_ALG_2:   return "PIN Verification Key (ANSI X9.132 algorithm 2)";
		case TR31_KEY_USAGE_PVK_X9_132_ALG_3:   return "PIN Verification Key (ANSI X9.132 algorithm 3)";
	}

	return "Unknown key usage value";
}

const char* tr31_get_key_algorithm_string(unsigned int algorithm)
{
	// see ANSI X9.143:2021, 6.3.2, table 3
	switch (algorithm) {
		case TR31_KEY_ALGORITHM_AES:    return "AES";
		case TR31_KEY_ALGORITHM_DES:    return "DES";
		case TR31_KEY_ALGORITHM_EC:     return "Elliptic Curve";
		case TR31_KEY_ALGORITHM_HMAC:   return "HMAC";
		case TR31_KEY_ALGORITHM_RSA:    return "RSA";
		case TR31_KEY_ALGORITHM_DSA:    return "DSA";
		case TR31_KEY_ALGORITHM_TDES:   return "TDES";
	}

	return "Unknown key algorithm value";
}

const char* tr31_get_key_mode_of_use_string(unsigned int mode_of_use)
{
	// see ANSI X9.143:2021, 6.3.3, table 4
	switch (mode_of_use) {
		case TR31_KEY_MODE_OF_USE_ENC_DEC:      return "Encrypt/Wrap and Decrypt/Unwrap";
		case TR31_KEY_MODE_OF_USE_MAC:          return "MAC Generate and Verify";
		case TR31_KEY_MODE_OF_USE_DEC:          return "Decrypt/Unwrap Only";
		case TR31_KEY_MODE_OF_USE_ENC:          return "Encrypt/Wrap Only";
		case TR31_KEY_MODE_OF_USE_MAC_GEN:      return "MAC Generate Only";
		case TR31_KEY_MODE_OF_USE_ANY:          return "No special restrictions";
		case TR31_KEY_MODE_OF_USE_SIG:          return "Signature Only";
		case TR31_KEY_MODE_OF_USE_MAC_VERIFY:   return "MAC Verify Only";
		case TR31_KEY_MODE_OF_USE_DERIVE:       return "Key Derivation";
		case TR31_KEY_MODE_OF_USE_VARIANT:      return "Create Key Variants";
	}

	return "Unknown key mode of use value";
}

const char* tr31_get_key_exportability_string(unsigned int exportability)
{
	// see ANSI X9.143:2021, 6.3.5, table 6
	switch (exportability) {
		case TR31_KEY_EXPORT_TRUSTED:           return "Exportable in a trusted key block only";
		case TR31_KEY_EXPORT_NONE:              return "Not exportable";
		case TR31_KEY_EXPORT_SENSITIVE:         return "Sensitive";
	}

	return "Unknown key exportability value";
}

const char* tr31_get_opt_block_id_ascii(unsigned int opt_block_id, char* ascii, size_t ascii_len)
{
	union {
		uint16_t value;
		char bytes[2];
	} opt_block_id_ascii;

	opt_block_id_ascii.value = htons(opt_block_id);

	if (ascii_len < 3) {
		return NULL;
	}
	for (size_t i = 0; i < sizeof(opt_block_id_ascii.bytes); ++i) {
		if (tr31_validate_format_an(opt_block_id_ascii.bytes, sizeof(opt_block_id_ascii.bytes)) == 0) {
			ascii[i] = opt_block_id_ascii.bytes[i];
		} else {
			ascii[i] = '?';
		}
	}
	ascii[2] = 0;

	return ascii;
}

const char* tr31_get_opt_block_id_string(unsigned int opt_block_id)
{
	// see ANSI X9.143:2021, 6.3.6, table 7
	switch (opt_block_id) {
		case TR31_OPT_BLOCK_AL:         return "Asymmetric Key Life (AKL)";
		case TR31_OPT_BLOCK_BI:         return "Base Derivation Key (BDK) Identifier";
		case TR31_OPT_BLOCK_CT:         return "Public Key Certificate";
		case TR31_OPT_BLOCK_DA:         return "Derivation(s) Allowed for Derivation Keys";
		case TR31_OPT_BLOCK_FL:         return "Flags";
		case TR31_OPT_BLOCK_HM:         return "Hash algorithm for HMAC";
		case TR31_OPT_BLOCK_IK:         return "Initial Key Identifier (IKID)";
		case TR31_OPT_BLOCK_KC:         return "Key Check Value (KCV) of wrapped key";
		case TR31_OPT_BLOCK_KP:         return "Key Check Value (KCV) of KBPK";
		case TR31_OPT_BLOCK_KS:         return "Initial Key Serial Number (KSN)";
		case TR31_OPT_BLOCK_KV:         return "Key Block Values";
		case TR31_OPT_BLOCK_LB:         return "Label";
		case TR31_OPT_BLOCK_PB:         return "Padding Block";
		case TR31_OPT_BLOCK_PK:         return "Protection Key Check Value (KCV) of export KBPK";
		case TR31_OPT_BLOCK_TC:         return "Time of Creation";
		case TR31_OPT_BLOCK_TS:         return "Time Stamp";
		case TR31_OPT_BLOCK_WP:         return "Wrapping Pedigree";
	}

	return "Unknown";
}
