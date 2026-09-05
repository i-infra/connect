// Nonolith Connect test support
// Fakes for the Session and ClientConn abstractions plus a synthetic
// StreamingDevice, allowing the REST and WebSocket handler stack to be
// exercised in-process without sockets or USB hardware.

#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>

#include "doctest.h"

#include "../dataserver.hpp"
#include "../json.hpp"
#include "../streaming_device/streaming_device.hpp"
#include "../streaming_device/stream_listener.hpp"

// Parse a JSON string, failing the test on parse errors.
inline JSONNode jparse(const std::string& s){
	return libjson::parse(s);
}

inline bool jhas(JSONNode& n, const char* prop){
	return n.find(prop) != n.end();
}

/// In-memory Session capturing everything the handlers write.
struct FakeSession: public Session {
	std::string method = "GET";
	std::string resource = "/";
	std::map<std::string, std::string> clientHeaders;
	std::map<std::string, std::string> responseHeaders;

	/// Body delivered to read_http_post_body callbacks
	std::string postBody;

	int status = -1;
	std::string body;
	bool responded = false;
	bool doneFlag = false;
	bool closed = false;

	std::string get_client_header(const std::string& key) const override {
		std::map<std::string, std::string>::const_iterator it = clientHeaders.find(key);
		return it == clientHeaders.end() ? "" : it->second;
	}
	const std::string& get_resource() const override { return resource; }
	const std::string& get_method() const override { return method; }

	void set_header(const std::string& k, const std::string& v) override {
		responseHeaders[k] = v;
	}
	void start_http(int s, const std::string& b = "", bool done = true) override {
		status = s;
		body += b;
		responded = true;
		doneFlag = done;
	}
	void read_http_post_body(std::function<void(std::string)> callback) override {
		callback(postBody);
	}
	void http_write(const std::string& b, bool done = false) override {
		body += b;
		if (done) doneFlag = true;
	}

	bool is_closed() const override { return closed; }

	// WebSocket surface: capture frames
	std::vector<std::string> wsMessages;
	std::vector<std::string> wsBinary;
	void send(const std::string& msg) override { wsMessages.push_back(msg); }
	void send_binary(const std::string& msg) override { wsBinary.push_back(msg); }

	JSONNode jsonBody(){ return jparse(body); }
};

typedef std::shared_ptr<FakeSession> fake_session_ptr;

/// ClientConn capturing broadcast/sent JSON and binary frames.
struct FakeClientConn: public ClientConn {
	std::vector<std::string> sent;
	std::vector<std::string> binaryFrames;

	void sendJSON(JSONNode& n) override { sent.push_back(n.write()); }
	void sendBinary(const std::string& d) override { binaryFrames.push_back(d); }

	void clear(){ sent.clear(); binaryFrames.clear(); }

	JSONNode last(){
		REQUIRE(!sent.empty());
		return jparse(sent.back());
	}

	/// All messages whose _action matches
	std::vector<JSONNode> actions(const std::string& action){
		std::vector<JSONNode> out;
		for (size_t i = 0; i < sent.size(); i++){
			JSONNode n = jparse(sent[i]);
			JSONNode::iterator it = n.find("_action");
			if (it != n.end() && it->as_string() == action) out.push_back(n);
		}
		return out;
	}
};

/// Synthetic streaming device shaped like the M1K (channels a/b, streams v/i).
class TestDevice: public StreamingDevice {
public:
	Channel channel_a, channel_b;
	Stream a_v, a_i, b_v, b_i;

	int resets = 0, starts = 0, pauses = 0, configures = 0;
	int lastInternalGain = 0;
	unsigned lastCurrentLimit = 0;

	TestDevice(double _sampleTime = 0.001, unsigned samples = 100, bool continuous = false):
		StreamingDevice(_sampleTime),
		channel_a("a", "A"), channel_b("b", "B"),
		a_v("v", "Voltage A", "V",  -5,   5,   1, 0.001, 1),
		a_i("i", "Current A", "mA", -200, 200, 2, 0.1,   1),
		b_v("v", "Voltage B", "V",  -5,   5,   1, 0.001, 1),
		b_i("i", "Current B", "mA", -200, 200, 2, 0.1,   1)
	{
		minSampleTime = 0.00001;
		currentLimit = 0;
		devMode = 0;
		rawMode = false;

		channels.push_back(&channel_a);
		channels.push_back(&channel_b);
		channel_a.streams.push_back(&a_v);
		channel_a.streams.push_back(&a_i);
		channel_b.streams.push_back(&b_v);
		channel_b.streams.push_back(&b_i);
		channel_a.source = makeConstantSource(0, 0);
		channel_b.source = makeConstantSource(0, 0);

		configure(0, _sampleTime, samples, continuous, false);
	}

	~TestDevice(){
		delete channel_a.source;
		delete channel_b.source;
	}

	const string serialno() override { return "TEST001"; }
	const string model() override { return "com.nonolithlabs.test"; }
	const string hwVersion() override { return "T1"; }
	const string fwVersion() override { return "9.9"; }

	void configure(int mode, double _sampleTime, unsigned samples,
	               bool continuous, bool raw) override {
		configures++;
		pause_capture();
		devMode = mode;
		sampleTime = _sampleTime;
		captureSamples = samples;
		captureContinuous = continuous;
		rawMode = raw;
		captureLength = samples * sampleTime;
		capture_i = capture_o = 0;
		a_v.allocate(samples);
		a_i.allocate(samples);
		b_v.allocate(samples);
		b_i.allocate(samples);
		notifyConfig();
	}

	void setInternalGain(Channel* c, Stream* s, int gain) override {
		lastInternalGain = gain;
		s->gain = gain;
		notifyGainChanged(c, s, gain);
	}

	void setCurrentLimit(unsigned limit) override {
		lastCurrentLimit = limit;
		currentLimit = limit;
	}

	/// Store one input sample on all four streams
	void feed(float av, float ai, float bv, float bi){
		put(a_v, av);
		put(a_i, ai);
		put(b_v, bv);
		put(b_i, bi);
		sampleDone();
	}

	/// Feed n samples of a deterministic ramp: a_v=k, a_i=10k, b_v=-k, b_i=100+k
	/// (k = absolute sample index), then finish the "packet".
	void feedRamp(unsigned n){
		for (unsigned k = 0; k < n; k++){
			float t = (float) capture_i;
			feed(t, t * 10, -t, 100 + t);
		}
		packetDone();
	}

	/// Advance the output counter as the hardware consuming samples would.
	void consumeOutput(unsigned n){
		for (unsigned k = 0; k < n; k++){
			if (channel_a.source) channel_a.source->getValue(capture_o, sampleTime);
			if (channel_b.source) channel_b.source->getValue(capture_o, sampleTime);
			capture_o++;
		}
	}

protected:
	void on_reset_capture() override { resets++; }
	void on_start_capture() override { starts++; }
	void on_pause_capture() override { pauses++; }
};

typedef boost::shared_ptr<TestDevice> test_device_ptr;

inline test_device_ptr makeTestDevice(double sampleTime = 0.001,
                                      unsigned samples = 100,
                                      bool continuous = false){
	return test_device_ptr(new TestDevice(sampleTime, samples, continuous));
}

/// RAII: register a device in the global `devices` set for the test's duration.
struct DeviceRegistration {
	device_ptr dev;
	DeviceRegistration(device_ptr d): dev(d){ devices.insert(dev); }
	~DeviceRegistration(){ devices.erase(dev); }
};

/// Issue a REST request against a device's handleREST (path relative to the
/// device, e.g. "/a/output?mode=1"). Returns the fake session; ->responded
/// distinguishes handled from unhandled (404) requests.
inline fake_session_ptr restRequest(device_ptr dev, const std::string& pathAndQuery,
                                    const std::string& method = "GET",
                                    const std::string& post = ""){
	fake_session_ptr s = std::make_shared<FakeSession>();
	s->method = method;
	s->resource = pathAndQuery;
	s->postBody = post;
	Url url(pathAndQuery);
	UrlPath path(url, 1);
	bool handled = dev->handleREST(path, s);
	if (!handled) s->status = 404; // mirrors rest_service fallthrough
	return s;
}

/// Issue a request through the top-level REST dispatcher (resource must
/// start with /rest/...), as server.cpp routes it.
void handleJSONRequest(UrlPath path, session_ptr client);
inline fake_session_ptr restServiceRequest(const std::string& resource,
                                           const std::string& method = "GET",
                                           const std::string& post = ""){
	fake_session_ptr s = std::make_shared<FakeSession>();
	s->method = method;
	s->resource = resource;
	s->postBody = post;
	Url url(resource);
	UrlPath path(url, 1);
	REQUIRE(path.matches("rest"));
	handleJSONRequest(path.sub(), s);
	return s;
}
