// Nonolith Connect
// https://github.com/nonolith/connect
// Session abstraction layer implementation
// Released under the terms of the GNU GPLv3+
// (C) 2012 Nonolith Labs, LLC

#include "session.hpp"
#include <iostream>

// ============================================================
// HttpSession
// ============================================================

HttpSession::HttpSession(beast::tcp_stream&& stream, http::request<http::string_body>&& req)
	: stream_(std::move(stream))
	, req_(std::move(req))
	, resource_(std::string(req_.target()))
	, method_(std::string(req_.method_string()))
	, headers_sent_(false)
	, closed_(false)
{}

HttpSession::~HttpSession() {
	if (!closed_) {
		boost::system::error_code ec;
		stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
		stream_.socket().close(ec);
	}
}

std::string HttpSession::get_client_header(const std::string& key) const {
	auto it = req_.find(key);
	if (it != req_.end())
		return std::string(it->value());
	return "";
}

const std::string& HttpSession::get_resource() const {
	return resource_;
}

const std::string& HttpSession::get_method() const {
	return method_;
}

void HttpSession::set_header(const std::string& key, const std::string& val) {
	response_headers_[key] = val;
}

void HttpSession::start_http(int status, const std::string& body, bool done) {
	if (closed_) return;

	if (!done) {
		// Streaming response: send headers with chunked transfer encoding,
		// then send the initial body chunk if non-empty.
		http::response<http::empty_body> res{static_cast<http::status>(status), req_.version()};
		res.set(http::field::server, "Nonolith Connect");
		res.set(http::field::transfer_encoding, "chunked");
		res.set(http::field::content_type, "text/plain");
		for (auto& kv : response_headers_)
			res.set(kv.first, kv.second);
		res.keep_alive(true);

		http::response_serializer<http::empty_body> sr{res};
		boost::system::error_code ec;
		http::write_header(stream_, sr, ec);
		if (ec) {
			closed_ = true;
			return;
		}
		headers_sent_ = true;

		if (!body.empty()) {
			http_write(body, false);
		}
	} else {
		// Complete response
		http::response<http::string_body> res{static_cast<http::status>(status), req_.version()};
		res.set(http::field::server, "Nonolith Connect");
		res.set(http::field::content_type, "application/json");
		for (auto& kv : response_headers_)
			res.set(kv.first, kv.second);
		res.keep_alive(false);
		res.body() = body;
		res.prepare_payload();

		boost::system::error_code ec;
		http::write(stream_, res, ec);
		if (ec) {
			closed_ = true;
			return;
		}

		// Close connection after complete response
		stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
		stream_.socket().close(ec);
		closed_ = true;
	}
}

void HttpSession::read_http_post_body(std::function<void(std::string)> callback) {
	// Beast reads the entire message at once, so body is already available
	callback(req_.body());
}

void HttpSession::http_write(const std::string& body, bool done) {
	if (closed_) return;

	boost::system::error_code ec;

	if (!body.empty()) {
		auto chunk = http::make_chunk(net::buffer(body));
		net::write(stream_, chunk, ec);
		if (ec) {
			closed_ = true;
			return;
		}
	}

	if (done) {
		auto last = http::make_chunk_last();
		net::write(stream_, last, ec);
		if (ec) {
			closed_ = true;
			return;
		}
		stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
		stream_.socket().close(ec);
		closed_ = true;
	}
}

bool HttpSession::is_closed() const {
	if (closed_) return true;
	return !stream_.socket().is_open();
}

void HttpSession::send(const std::string&) {
	// Not applicable for HTTP sessions
	std::cerr << "Error: send() called on HTTP session" << std::endl;
}

beast::tcp_stream HttpSession::release_stream() {
	return std::move(stream_);
}


// ============================================================
// WsSession
// ============================================================

WsSession::WsSession(websocket::stream<beast::tcp_stream>&& ws,
                      const http::request<http::string_body>& original_req)
	: ws_(std::move(ws))
	, resource_(std::string(original_req.target()))
	, method_(std::string(original_req.method_string()))
	, closed_(false)
{
	// Copy headers from original request
	for (auto const& field : original_req) {
		headers_[std::string(field.name_string())] = std::string(field.value());
	}
}

WsSession::~WsSession() {
	if (!closed_ && ws_.is_open()) {
		boost::system::error_code ec;
		ws_.close(websocket::close_code::normal, ec);
	}
}

std::string WsSession::get_client_header(const std::string& key) const {
	auto it = headers_.find(key);
	if (it != headers_.end())
		return it->second;
	return "";
}

const std::string& WsSession::get_resource() const {
	return resource_;
}

const std::string& WsSession::get_method() const {
	return method_;
}

bool WsSession::is_closed() const {
	if (closed_) return true;
	return !ws_.is_open();
}

void WsSession::send(const std::string& msg) {
	if (closed_ || !ws_.is_open()) return;
	boost::system::error_code ec;
	ws_.text(true);
	ws_.write(net::buffer(msg), ec);
	if (ec) {
		closed_ = true;
	}
}

bool WsSession::read_message(std::string& out) {
	if (closed_ || !ws_.is_open()) return false;
	beast::flat_buffer buffer;
	boost::system::error_code ec;
	ws_.read(buffer, ec);
	if (ec) {
		closed_ = true;
		return false;
	}
	out = beast::buffers_to_string(buffer.data());
	return true;
}
