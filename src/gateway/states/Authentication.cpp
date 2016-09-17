/*
 * Copyright (c) 2016 Ember
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "Authentication.h"
#include "../AccountService.h"
#include "../Config.h"
#include "../RealmQueue.h"
#include "../ClientConnection.h"
#include "../Locator.h"
#include "../EventDispatcher.h"
#include <game_protocol/Opcodes.h>
#include <game_protocol/PacketHeaders.h>
#include <game_protocol/Packets.h>
#include <spark/Buffer.h>
#include <spark/temp/Account_generated.h>
#include <shared/util/xoroshiro128plus.h>
#include <logger/Logging.h>
#include <botan/botan.h>
#include <botan/sha160.h>
#include <cstdint>

namespace em = ember::messaging;

namespace ember { namespace authentication {

void send_auth_challenge(ClientContext* ctx);
void send_auth_result(ClientContext* ctx, protocol::ResultCode result);
void handle_authentication(ClientContext* ctx);
void prove_session(ClientContext* ctx, Botan::BigInt key, const protocol::CMSG_AUTH_SESSION& packet);
void fetch_session_key(ClientContext* ctx, const protocol::CMSG_AUTH_SESSION& packet);
void fetch_account_id(ClientContext* ctx, const protocol::CMSG_AUTH_SESSION& packet);

void handle_authentication(ClientContext* ctx) {
	LOG_TRACE_FILTER_GLOB(LF_NETWORK) << __func__ << LOG_ASYNC;

	// prevent repeated auth attempts
	if(ctx->auth_status != AuthStatus::NOT_AUTHED) {
		return;
	}
	
	ctx->auth_status = AuthStatus::IN_PROGRESS;

	protocol::CMSG_AUTH_SESSION packet;
	packet.set_size(ctx->header->size - sizeof(protocol::ClientHeader::opcode));

	if(!ctx->handler->packet_deserialise(packet, *ctx->buffer)) {
		return;
	}

	LOG_DEBUG_GLOB << "Received session proof from " << packet.username << LOG_ASYNC;

	// todo - check game build
	fetch_account_id(ctx, packet);
}

void fetch_account_id(ClientContext* ctx, const protocol::CMSG_AUTH_SESSION& packet) {
	LOG_TRACE_FILTER_GLOB(LF_NETWORK) << __func__ << LOG_ASYNC;

	auto uuid = ctx->handler->uuid();

	Locator::account()->locate_account_id(packet.username,
	                                      [uuid, packet](auto status, auto id) {
		auto event = std::make_unique<AccountIDResponse>(packet, status, id);
		Locator::dispatcher()->post_event(uuid, std::move(event));
	});
}

void handle_account_id(ClientContext* ctx, const AccountIDResponse* event) {
	LOG_TRACE_FILTER_GLOB(LF_NETWORK) << __func__ << LOG_ASYNC;

	if(event->status != em::account::Status::OK) {
		LOG_ERROR_FILTER_GLOB(LF_NETWORK)
			<< "Account server returned " << em::account::EnumNameStatus(event->status)
			<< " for " << event->packet.username << " lookup" << LOG_ASYNC;
		ctx->connection->close_session();
		return;
	}

	if(event->id) {
		ctx->account_id = event->id;
		fetch_session_key(ctx, event->packet);
	} else {
		LOG_DEBUG_FILTER_GLOB(LF_NETWORK) << "Account ID lookup for failed for "
			<< event->packet.username << LOG_ASYNC;
	}
}

void fetch_session_key(ClientContext* ctx, const protocol::CMSG_AUTH_SESSION& packet) {
	LOG_TRACE_FILTER_GLOB(LF_NETWORK) << __func__ << LOG_ASYNC;

	auto uuid = ctx->handler->uuid();

	Locator::account()->locate_session(ctx->account_id,
	                                   [uuid, packet](auto status, auto key) {
		auto event = std::make_unique<SessionKeyResponse>(packet, status, key);
		Locator::dispatcher()->post_event(uuid, std::move(event));
	});
}

void handle_session_key(ClientContext* ctx, const SessionKeyResponse* event) {
	LOG_DEBUG_FILTER_GLOB(LF_NETWORK)
		<< "Account server returned " << em::account::EnumNameStatus(event->status)
		<< " for " << event->packet.username << LOG_ASYNC;

	ctx->auth_status = AuthStatus::FAILED; // default unless overridden by success

	if(event->status != em::account::Status::OK) {
		protocol::ResultCode result;

		switch(event->status) {
			case em::account::Status::ALREADY_LOGGED_IN:
				result = protocol::ResultCode::AUTH_ALREADY_ONLINE;
				break;
			case em::account::Status::SESSION_NOT_FOUND:
				result = protocol::ResultCode::AUTH_UNKNOWN_ACCOUNT;
				break;
			default:
				LOG_ERROR_FILTER_GLOB(LF_NETWORK) << "Received "
					<< em::account::EnumNameStatus(event->status)
					<< " from account server" << LOG_ASYNC;
				result = protocol::ResultCode::AUTH_SYSTEM_ERROR;
		}

		// note: the game doesn't seem to pay attention to this
		send_auth_result(ctx, result);
	} else {
		prove_session(ctx, event->key, event->packet);
	}
}

void send_auth_challenge(ClientContext* ctx) {
	LOG_TRACE_FILTER_GLOB(LF_NETWORK) << __func__ << LOG_ASYNC;
	protocol::SMSG_AUTH_CHALLENGE response;
	response.seed = ctx->auth_seed = static_cast<std::uint32_t>(ember::rng::xorshift::next());
	ctx->connection->send(response);
}

void send_addon_data(ClientContext* ctx, const protocol::CMSG_AUTH_SESSION& packet) {
	LOG_TRACE_FILTER_GLOB(LF_NETWORK) << __func__ << LOG_ASYNC;

	protocol::SMSG_ADDON_INFO response;

	// todo, use AddonData.dbc
	for(auto& addon : packet.addons) {
		LOG_DEBUG_GLOB << "Addon: " << addon.name << ", Key version: " << addon.key_version
			<< ", CRC: " << addon.crc << ", URL CRC: " << addon.update_url_crc << LOG_ASYNC;

		protocol::SMSG_ADDON_INFO::AddonData data;
		data.type = protocol::SMSG_ADDON_INFO::AddonData::Type::BLIZZARD;
		data.update_available_flag = false; // URL must be present for this to work (check URL CRC)

		if(addon.crc != 0x4C1C776D || addon.key_version != 1) { // todo, define?
			LOG_DEBUG_GLOB << "Repairing " << addon.name << "..." << LOG_ASYNC;
			data.key_version = 1;
		} else {
			data.key_version = 0;
		}		
		
		response.addon_data.emplace_back(std::move(data));
	}

	ctx->connection->send(response);
}

void prove_session(ClientContext* ctx, Botan::BigInt key, const protocol::CMSG_AUTH_SESSION& packet) {
	LOG_TRACE_FILTER_GLOB(LF_NETWORK) << __func__ << LOG_ASYNC;

	Botan::SecureVector<Botan::byte> k_bytes = Botan::BigInt::encode(key);
	std::uint32_t unknown = 0; // this is hardcoded to zero in the client

	Botan::SHA_160 hasher;
	hasher.update(packet.username);
	hasher.update_be(boost::endian::native_to_big(unknown));
	hasher.update_be(boost::endian::native_to_big(packet.seed));
	hasher.update_be(boost::endian::native_to_big(ctx->auth_seed));
	hasher.update(k_bytes);
	Botan::SecureVector<Botan::byte> calc_hash = hasher.final();

	if(calc_hash != packet.digest) {
		LOG_DEBUG_GLOB << "Received bad digest from " << packet.username << LOG_ASYNC;
		send_auth_result(ctx, protocol::ResultCode::AUTH_BAD_SERVER_PROOF);
		return;
	}

	ctx->connection->set_authenticated(key);
	ctx->account_name = packet.username;
	ctx->auth_status = AuthStatus::SUCCESS;

	/*
	* Note: MaNGOS claims you need a full auth packet for the initial AUTH_WAIT_QUEUE
	* but that doesn't seem to be true - if this bugs out, check that out
	*/
	unsigned int active_players = 0; // todo, keeping accurate player counts will involve the world server

	if(active_players >= Locator::config()->max_slots) {
		auto uuid = ctx->handler->uuid();

		Locator::queue()->enqueue(uuid,
			[uuid, packet](std::size_t position) {
				Locator::dispatcher()->post_event(uuid, QueuePosition(position));
			},
			[uuid, packet]() {
				auto event = std::make_unique<QueueSuccess>(std::move(packet));
				Locator::dispatcher()->post_event(uuid, std::move(event));
			}
		);

		ctx->handler->state_update(ClientState::IN_QUEUE);
		return;
	}

	auth_success(ctx, packet);
}

void auth_success(ClientContext* ctx, const protocol::CMSG_AUTH_SESSION& packet) {
	LOG_TRACE_FILTER_GLOB(LF_NETWORK) << __func__ << LOG_ASYNC;
	send_auth_result(ctx, protocol::ResultCode::AUTH_OK);
	send_addon_data(ctx, packet);
	ctx->handler->state_update(ClientState::CHARACTER_LIST);
}

void send_auth_result(ClientContext* ctx, protocol::ResultCode result) {
	LOG_TRACE_FILTER_GLOB(LF_NETWORK) << __func__ << LOG_ASYNC;

	// not convinced that this packet is correct, apart from for AUTH_OK
	protocol::SMSG_AUTH_RESPONSE response;
	response.result = result;
	ctx->connection->send(response);
}

void enter(ClientContext* ctx) {
	send_auth_challenge(ctx);
}

void handle_packet(ClientContext* ctx) {
	switch(ctx->header->opcode) {
		case protocol::ClientOpcodes::CMSG_AUTH_SESSION:
			handle_authentication(ctx);
			break;
		//default:
			//ctx->state = ClientState::UNEXPECTED_PACKET;
	}
}

void handle_event(ClientContext* ctx, const Event* event) {
	switch(event->type) {
		case EventType::ACCOUNT_ID_RESPONSE:
			handle_account_id(ctx, static_cast<const AccountIDResponse*>(event));
			break;
		case EventType::SESSION_KEY_RESPONSE:
			handle_session_key(ctx, static_cast<const SessionKeyResponse*>(event));
			break;
	}
}

void exit(ClientContext* ctx) {
	// don't care
}

}} // authentication, ember