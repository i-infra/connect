// Nonolith Connect
// https://github.com/nonolith/connect
// Server initialization and request dispatch
// Released under the terms of the GNU GPLv3+
// (C) 2012 Nonolith Labs, LLC
// Authors:
//   Kevin Mehall <km@kevinmehall.net>

#include <iostream>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/regex.hpp>

#include "dataserver.hpp"
#include "websocket_handler.hpp"
#include "url.hpp"

using boost::asio::ip::tcp;
static unsigned short port = 9003;

std::set<device_ptr> devices;
boost::asio::io_context io;

bool debugFlag = false;
bool allowRemote = false;
bool allowAnyOrigin = false;

Event device_list_changed;
Event capture_state_changed;

void handleJSONRequest(UrlPath path, session_ptr client);

static void do_session(beast::tcp_stream&& stream);

static void do_accept(tcp::acceptor& acceptor) {
	acceptor.async_accept(
		net::make_strand(io),
		[&acceptor](boost::system::error_code ec, tcp::socket socket) {
			if (!ec) {
				boost::asio::post(io, [s = beast::tcp_stream(std::move(socket))]() mutable {
					do_session(std::move(s));
				});
			}
			do_accept(acceptor);
		});
}

static void do_session(beast::tcp_stream&& stream) {
	try {
		beast::flat_buffer buffer;
		http::request<http::string_body> req;

		stream.expires_after(std::chrono::seconds(30));
		http::read(stream, buffer, req);
		stream.expires_never();

		const std::string resource(req.target());

		// Check if this is a WebSocket upgrade to /ws
		if (websocket::is_upgrade(req)) {
			Url url(resource);
			UrlPath path(url, 1);
			if (!path.leaf() && path.matches("ws")) {
				websocket::stream<beast::tcp_stream> ws(std::move(stream));
				ws.set_option(websocket::stream_base::decorator(
					[](websocket::response_type& res) {
						res.set(http::field::server, "Nonolith Connect");
					}));
				ws.accept(req);

				auto ws_session = std::make_shared<WsSession>(std::move(ws), req);
				ws_on_open(ws_session);

				// Read loop runs on a dedicated thread so it doesn't block
				// the io_context. Messages and close are posted back to the
				// io_context for thread-safe processing.
				// Async read loop — runs on io_context so it doesn't
				// block USB data callbacks from being dispatched.
				ws_session->start_async_read(
					[](std::shared_ptr<WsSession> s, std::string msg) {
						ws_on_message(s, msg);
					},
					[](std::shared_ptr<WsSession> s) {
						ws_on_close(s);
					});
				return;
			}
		}

		// HTTP request
		auto session = std::make_shared<HttpSession>(std::move(stream), std::move(req));
		on_client_connect(session);

	} catch (boost::system::system_error& se) {
		if (se.code() != http::error::end_of_stream)
			std::cerr << "HTTP error: " << se.what() << std::endl;
	} catch (std::exception& e) {
		std::cerr << "Session error: " << e.what() << std::endl;
	}
}

int main(int argc, char* argv[]) {
	try {
		usb_init();

		for (int i = 1; i < argc; i++) {
			string arg(argv[i]);
			if (arg == "debug") debugFlag = true;
			if (arg == "allow-remote") allowRemote = true;
			if (arg == "allow-any-origin") allowAnyOrigin = true;
			if (arg.rfind("port=", 0) == 0) port = atoi(arg.c_str() + 5);
		}

		boost::asio::ip::address_v4 bind_addr;
		if (!allowRemote) bind_addr = boost::asio::ip::address_v4::loopback();

		tcp::endpoint endpoint(bind_addr, port);
		tcp::acceptor acceptor(io, endpoint);
		acceptor.set_option(net::socket_base::reuse_address(true));

		do_accept(acceptor);

		usb_scan_devices();

		io.run();
	} catch (std::exception& e) {
		std::cerr << "Exception: " << e.what() << std::endl;
	}

	return 0;
}

const string redir_url = "http://www.nonolithlabs.com/connect/?utm_source=connect&utm_medium=app&utm_campaign=server-redir";

void on_client_connect(session_ptr client) {
	static const boost::regex nonolith_domain("^https?://[[:w:]\\.-]*?nonolithlabs.com$");
	static const boost::regex localhost_domain("^https?://localhost(:[[:d:]]+)?$");
	const string origin = client->get_client_header("Origin");

	if (!allowAnyOrigin
			&& origin != ""
			&& origin != "null"
			&& !regex_match(origin, localhost_domain)
			&& !regex_match(origin, nonolith_domain)
			) {
		client->start_http(403, "Origin not allowed");
		std::cerr << "Rejected client with unknown origin " << origin << std::endl;
		return;
	}

	const std::string resource = client->get_resource();
	Url url(resource);
	UrlPath path(url, 1);

	try {
		if (path.leaf()) { // "/"
			client->set_header("Location", redir_url);
			client->start_http(301);
		} else if (path.matches("rest")) {
			handleJSONRequest(path.sub(), client);
		} else {
			client->start_http(404, "Not found");
		}
	} catch (std::exception& e) {
		JSONNode j;
		j.push_back(JSONNode("error", e.what()));
		std::cerr << "Exception while processing request: " << e.what() << std::endl;
		respondJSON(client, j, 500);
	}
}
