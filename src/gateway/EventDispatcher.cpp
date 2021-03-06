/*
 * Copyright (c) 2016 Ember
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "EventDispatcher.h"
#include <logger/Logging.h>
#include <type_traits>

namespace ember {

thread_local EventDispatcher::HandlerMap EventDispatcher::handlers_;

void EventDispatcher::post_event(const ClientUUID& client, std::unique_ptr<Event> event) const {
	auto service = pool_.get_service(client.service());

	// bad service index encoded in the UUID
	if(service == nullptr) {
		LOG_ERROR_GLOB << "Invalid service index, " << client.service() << LOG_ASYNC;
		return;
	}

	// this is a workaround for ASIO's lack of C++14 support,
	// otherwise we'd ideally just move event into the lambda
	auto raw = event.release();

	service->post([client, raw] {
		auto event = std::unique_ptr<const Event>(raw);
		auto handler = handlers_.find(client);

		// client disconnected, nothing to do here
		if(handler == handlers_.end()) {
			LOG_DEBUG_GLOB << "Client disconnected, event discarded" << LOG_ASYNC;
			return;
		}

		handler->second->handle_event(std::move(event));
	});
}

void EventDispatcher::post_event(const ClientUUID& client,
                                 std::shared_ptr<const Event>& event) const {
	auto service = pool_.get_service(client.service());

	// bad service index encoded in the UUID
	if(service == nullptr) {
		LOG_ERROR_GLOB << "Invalid service index, " << client.service() << LOG_ASYNC;
		return;
	}

	service->post([=] {
		auto handler = handlers_.find(client);

		// client disconnected, nothing to do here
		if(handler == handlers_.end()) {
			LOG_DEBUG_GLOB << "Client disconnected, event discarded" << LOG_ASYNC;
			return;
		}
		
		handler->second->handle_event(event);
	});
}

void EventDispatcher::register_handler(ClientHandler* handler) {
	auto service = pool_.get_service(handler->uuid().service());

	service->dispatch([=] {
		handlers_[handler->uuid()] = handler;
	});
}

void EventDispatcher::remove_handler(ClientHandler* handler) {
	auto service = pool_.get_service(handler->uuid().service());
	
	service->dispatch([=] {
		handlers_.erase(handler->uuid());
	});
}

} // ember