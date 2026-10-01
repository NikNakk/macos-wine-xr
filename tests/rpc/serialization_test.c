// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "openxr_rpc_generated.h"
#include <stdio.h>
#include <stdlib.h>
#define REQUIRE(x)                                                                                                     \
	do {                                                                                                           \
		if (!(x)) {                                                                                            \
			fprintf(stderr, "failed: %s at %d\n", #x, __LINE__);                                           \
			return 1;                                                                                      \
		}                                                                                                      \
	} while (0)
int main(void) {
	struct mwxr_wire_request in = {0}, out = {0};
	uint8_t encoded[MWXR_WIRE_REQUEST_SIZE], again[MWXR_WIRE_REQUEST_SIZE];
	in.object = UINT64_C(0xfedcba9876543210);
	in.time = -123456789;
	in.pose.orientation[0] = -1.5f;
	in.ids[63] = UINT64_MAX;
	strcpy(in.name, "/user/hand/left");
	in.views[3].array_index = 0xffffffffu;
	mwxr_encode_request(encoded, &in);
	REQUIRE(encoded[0] == 0x10 && encoded[7] == 0xfe);
	mwxr_decode_request(encoded, &out);
	REQUIRE(out.object == in.object && out.time == in.time && out.pose.orientation[0] == -1.5f &&
		out.ids[63] == UINT64_MAX && !strcmp(in.name, out.name));
	mwxr_encode_request(again, &out);
	REQUIRE(!memcmp(encoded, again, sizeof(encoded)));
	struct mwxr_wire_response r = {0}, decoded = {0};
	uint8_t data[MWXR_WIRE_RESPONSE_SIZE], reencoded[MWXR_WIRE_RESPONSE_SIZE];
	r.formats[63] = -7;
	r.images[15].id = UINT64_MAX;
	r.images[15].strategy = 2;
	strcpy(r.images[15].name, "dxmt-native-boundary");
	r.views[3].fov[0] = -0.75f;
	mwxr_encode_response(data, &r);
	mwxr_decode_response(data, &decoded);
	mwxr_encode_response(reencoded, &decoded);
	REQUIRE(!memcmp(data, reencoded, sizeof(data)));
	REQUIRE(decoded.formats[63] == -7 && decoded.images[15].id == UINT64_MAX && decoded.views[3].fov[0] == -0.75f);
	REQUIRE(!mwxr_rpc_operation_valid(0) && !mwxr_rpc_operation_valid(UINT32_MAX));
	puts("fixed-width RPC boundary codecs passed");
	return 0;
}
