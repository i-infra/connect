// Nonolith Connect
// https://github.com/nonolith/connect
// Websocket handler declarations
// Released under the terms of the GNU GPLv3+
// (C) 2012 Nonolith Labs, LLC
// Authors:
//   Kevin Mehall <km@kevinmehall.net>

#pragma once

#include "session.hpp"
#include <map>
#include <string>
#include <vector>

// WebSocket connection lifecycle — called from server.cpp
void ws_on_open(session_ptr client);
void ws_on_close(session_ptr client);
void ws_on_message(session_ptr client, const std::string &msg);
void ws_on_message(session_ptr client, const std::vector<unsigned char> &data);

// HTTP dispatch — called from server.cpp
void on_client_connect(session_ptr client);
