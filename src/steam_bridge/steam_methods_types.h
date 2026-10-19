/* Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * What the generated method table (steam_methods.h) says about a Steam
 * interface method. Methods absent from the table take and return integers
 * and pointers only, and are forwarded as they are. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define STEAM_MAX_PARAMS 10

/* ARG_STRUCT: a structure by value, or a game object the client would call back (refused). */
enum steam_arg { ARG_INT, ARG_F32, ARG_F64, ARG_STRUCT };

enum steam_ret {
	RET_INT,    /* rax in both conventions (void, integers, pointers, bool) */
	RET_F32,    /* xmm0 in both */
	RET_F64,    /* xmm0 in both */
	RET_CLASS8, /* CSteamID/CGameID: Microsoft passes a hidden result pointer after `this`, System V uses rax */
	RET_STRUCT, /* any other structure by value: not bridged */
};

enum steam_special {
	SPECIAL_NONE,
	FN_INT_STRING, /* a void (*)(int, const char *) hook: queued with a copy of the text */
	FN_ALWAYS_ONE, /* steam_api's "is this callback registered" query: always yes, as Proton does */
	FN_NOTHING,    /* retired post-result hook: a function that does nothing */
	FN_DEFERRED,   /* another function pointer: not bridged, NULL is passed */
	MSG_SEND,      /* SendMessages: messages go back to the Mac client */
	MSG_RECEIVE,   /* ReceiveMessagesOn*: messages come from the Mac client */
	MSG_ALLOCATE,  /* AllocateMessage: one message comes from the Mac client */
};

struct steam_method {
	uint8_t index;
	uint8_t ret;     /* enum steam_ret */
	uint8_t special; /* enum steam_special */
	uint8_t fnarg;   /* the parameter holding the function pointer, 0xff for none */
	uint8_t nargs;
	uint8_t args[STEAM_MAX_PARAMS]; /* enum steam_arg, excluding `this` */
	const char *name;
};

struct steam_interface {
	const char *version; /* "SteamUser023" */
	const char *classname;
	uint16_t method_count;
	uint16_t described_count;
	const struct steam_method *methods;
};
