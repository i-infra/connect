// Nonolith Connect
// https://github.com/nonolith/connect
// Session abstraction layer (replaces websocketpp)
// Released under the terms of the GNU GPLv3+
// (C) 2012 Nonolith Labs, LLC

#pragma once

#include <string>
#include <map>
#include <queue>
#include <memory>
#include <functional>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class Session;
typedef std::shared_ptr<Session> session_ptr;

/// Base class providing the same API surface as the old websocketpp::session_ptr
class Session {
public:
	virtual ~Session() {}

	virtual std::string get_client_header(const std::string& key) const = 0;
	virtual const std::string& get_resource() const = 0;
	virtual const std::string& get_method() const = 0;

	virtual void set_header(const std::string& key, const std::string& val) = 0;
	virtual void start_http(int status, const std::string& body = "", bool done = true) = 0;
	virtual void read_http_post_body(std::function<void(std::string)> callback) = 0;
	virtual void http_write(const std::string& body, bool done = false) = 0;

	virtual bool is_closed() const = 0;

	// WebSocket only
	virtual void send(const std::string& msg) = 0;
	virtual void send_binary(const std::string& msg) { send(msg); }

	// Called on WS upgrade — not used by most code
	virtual void start_websocket() {}
};


/// HTTP session backed by Boost.Beast
class HttpSession : public Session, public std::enable_shared_from_this<HttpSession> {
public:
	HttpSession(beast::tcp_stream&& stream, http::request<http::string_body>&& req);
	~HttpSession();

	std::string get_client_header(const std::string& key) const override;
	const std::string& get_resource() const override;
	const std::string& get_method() const override;

	void set_header(const std::string& key, const std::string& val) override;
	void start_http(int status, const std::string& body = "", bool done = true) override;
	void read_http_post_body(std::function<void(std::string)> callback) override;
	void http_write(const std::string& body, bool done = false) override;

	bool is_closed() const override;
	void send(const std::string& msg) override;

	/// Detach the stream for WebSocket upgrade
	beast::tcp_stream release_stream();
	http::request<http::string_body>& request() { return req_; }

private:
	beast::tcp_stream stream_;
	http::request<http::string_body> req_;
	std::string resource_;
	std::string method_;
	std::map<std::string, std::string> response_headers_;
	bool headers_sent_;
	bool closed_;
};


/// WebSocket session backed by Boost.Beast
class WsSession : public Session, public std::enable_shared_from_this<WsSession> {
public:
	WsSession(websocket::stream<beast::tcp_stream>&& ws,
	          const http::request<http::string_body>& original_req);
	~WsSession();

	std::string get_client_header(const std::string& key) const override;
	const std::string& get_resource() const override;
	const std::string& get_method() const override;

	void set_header(const std::string&, const std::string&) override {}
	void start_http(int, const std::string&, bool) override {}
	void read_http_post_body(std::function<void(std::string)>) override {}
	void http_write(const std::string&, bool) override {}

	bool is_closed() const override;
	void send(const std::string& msg) override;
	void send_binary(const std::string& msg) override;

	/// Start async read loop on the io_context.
	/// Calls on_message for each received message, on_close when done.
	void start_async_read(std::function<void(std::shared_ptr<WsSession>, std::string)> on_message,
	                      std::function<void(std::shared_ptr<WsSession>)> on_close);

private:
	void do_read();
	void do_write();

	websocket::stream<beast::tcp_stream> ws_;
	beast::flat_buffer read_buf_;
	std::string resource_;
	std::string method_;
	std::map<std::string, std::string> headers_;
	std::function<void(std::shared_ptr<WsSession>, std::string)> on_message_;
	std::function<void(std::shared_ptr<WsSession>)> on_close_;
	std::queue<std::pair<std::string, bool>> write_queue_; // (payload, is_binary)
	bool writing_;
	bool closed_;
};
