// Tests for the WebSocket connection layer (websocket_service.cpp):
// hello/devices lifecycle, selectDevice, command dispatch, error replies.

#include "support.hpp"
#include "../websocket_handler.hpp"

namespace {

struct WsClient {
	fake_session_ptr session;

	WsClient(){
		session = std::make_shared<FakeSession>();
		ws_on_open(session);
	}
	~WsClient(){ ws_on_close(session); }

	void send(const std::string& msg){ ws_on_message(session, msg); }

	std::vector<JSONNode> actions(const std::string& action){
		std::vector<JSONNode> out;
		for (size_t i = 0; i < session->wsMessages.size(); i++){
			JSONNode n = jparse(session->wsMessages[i]);
			JSONNode::iterator it = n.find("_action");
			if (it != n.end() && it->as_string() == action) out.push_back(n);
		}
		return out;
	}

	void clear(){ session->wsMessages.clear(); session->wsBinary.clear(); }
};

}

TEST_SUITE("websocket_service"){

TEST_CASE("connection receives serverHello then device list"){
	WsClient c;
	REQUIRE(c.session->wsMessages.size() == 2);

	JSONNode hello = jparse(c.session->wsMessages[0]);
	CHECK(hello.at("_action").as_string() == "serverHello");
	CHECK(hello.at("server").as_string() == "Nonolith Connect");
	CHECK(jhas(hello, "version"));
	CHECK(jhas(hello, "gitVersion"));

	JSONNode devs = jparse(c.session->wsMessages[1]);
	CHECK(devs.at("_action").as_string() == "devices");
	CHECK(devs.at("devices").size() == 0);
}

TEST_CASE("device list updates are pushed on device_list_changed"){
	test_device_ptr dev = makeTestDevice();
	WsClient c;
	c.clear();

	{
		DeviceRegistration reg(dev);
		device_list_changed.notify();
		std::vector<JSONNode> msgs = c.actions("devices");
		REQUIRE(msgs.size() == 1);
		JSONNode d = msgs[0].at("devices").at("com.nonolithlabs.test~TEST001");
		CHECK(d.at("model").as_string() == "com.nonolithlabs.test");
	}

	device_list_changed.notify(); // device now gone
	std::vector<JSONNode> msgs = c.actions("devices");
	REQUIRE(msgs.size() == 2);
	CHECK(msgs[1].at("devices").size() == 0);
}

TEST_CASE("selectDevice attaches and sends deviceConfig"){
	test_device_ptr dev = makeTestDevice();
	DeviceRegistration reg(dev);
	WsClient c;
	c.clear();

	c.send("{\"_cmd\": \"selectDevice\", \"id\": \"com.nonolithlabs.test~TEST001\"}");
	std::vector<JSONNode> dc = c.actions("deviceConfig");
	REQUIRE(dc.size() == 1);
	CHECK(dc[0].at("device").at("serial").as_string() == "TEST001");
	CHECK(dev->connections.size() == 1);
}

TEST_CASE("selectDevice with unknown id is ignored without crashing"){
	WsClient c;
	c.clear();
	c.send("{\"_cmd\": \"selectDevice\", \"id\": \"nope~X\"}");
	CHECK(c.session->wsMessages.empty());
}

TEST_CASE("commands before selectDevice are dropped"){
	WsClient c;
	c.clear();
	c.send("{\"_cmd\": \"startCapture\"}");
	CHECK(c.session->wsMessages.empty());
}

TEST_CASE("commands are dispatched to the selected device"){
	test_device_ptr dev = makeTestDevice();
	DeviceRegistration reg(dev);
	WsClient c;
	c.send("{\"_cmd\": \"selectDevice\", \"id\": \"com.nonolithlabs.test~TEST001\"}");
	c.clear();

	c.send("{\"_cmd\": \"startCapture\"}");
	CHECK(dev->captureState == true);
	std::vector<JSONNode> cs = c.actions("captureState");
	REQUIRE(cs.size() == 1);
	CHECK(cs[0].at("state").as_bool() == true);

	c.send("{\"_cmd\": \"pauseCapture\"}");
	CHECK(dev->captureState == false);
}

TEST_CASE("full streaming round trip over the WS layer"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	DeviceRegistration reg(dev);
	WsClient c;
	c.send("{\"_cmd\": \"selectDevice\", \"id\": \"com.nonolithlabs.test~TEST001\"}");
	c.clear();

	c.send("{\"_cmd\": \"listen\", \"id\": 1, \"count\": 2, \"decimateFactor\": 1, "
		"\"start\": 0, \"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");
	dev->feedRamp(5);

	std::vector<JSONNode> ups = c.actions("update");
	REQUIRE(!ups.empty());
	CHECK(ups[0].at("id").as_int() == 1);
	CHECK(ups[0].at("data")[0][0].as_float() == doctest::Approx(0));
}

TEST_CASE("binary listener updates arrive as binary frames"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	DeviceRegistration reg(dev);
	WsClient c;
	c.send("{\"_cmd\": \"selectDevice\", \"id\": \"com.nonolithlabs.test~TEST001\"}");
	c.clear();

	c.send("{\"_cmd\": \"listen\", \"id\": 3, \"binary\": true, \"count\": 1, "
		"\"decimateFactor\": 1, \"start\": 0, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");
	dev->feedRamp(3);

	CHECK(c.actions("update").empty());
	REQUIRE(c.session->wsBinary.size() == 1);
	CHECK((uint8_t) c.session->wsBinary[0][0] == 1);
}

TEST_CASE("invalid JSON produces an error action"){
	WsClient c;
	c.clear();
	c.send("this is not json");
	std::vector<JSONNode> errs = c.actions("error");
	REQUIRE(errs.size() == 1);
	CHECK(jhas(errs[0], "error"));
}

TEST_CASE("missing _cmd produces an error action"){
	WsClient c;
	c.clear();
	c.send("{\"foo\": 1}");
	CHECK(c.actions("error").size() == 1);
}

TEST_CASE("command errors echo the request id"){
	test_device_ptr dev = makeTestDevice();
	DeviceRegistration reg(dev);
	WsClient c;
	c.send("{\"_cmd\": \"selectDevice\", \"id\": \"com.nonolithlabs.test~TEST001\"}");
	c.clear();

	// listen with an id but a bad stream reference throws inside processMessage
	c.send("{\"_cmd\": \"listen\", \"id\": 7, \"count\": 1, "
		"\"streams\": [{\"channel\": \"z\", \"stream\": \"v\"}]}");
	std::vector<JSONNode> errs = c.actions("error");
	REQUIRE(errs.size() == 1);
	CHECK(errs[0].at("id").as_int() == 7);
}

TEST_CASE("unknown command is silently logged, no reply"){
	test_device_ptr dev = makeTestDevice();
	DeviceRegistration reg(dev);
	WsClient c;
	c.send("{\"_cmd\": \"selectDevice\", \"id\": \"com.nonolithlabs.test~TEST001\"}");
	c.clear();
	c.send("{\"_cmd\": \"definitelyNotACommand\"}");
	CHECK(c.session->wsMessages.empty());
}

TEST_CASE("closing the connection detaches from the device"){
	test_device_ptr dev = makeTestDevice();
	DeviceRegistration reg(dev);
	{
		WsClient c;
		c.send("{\"_cmd\": \"selectDevice\", \"id\": \"com.nonolithlabs.test~TEST001\"}");
		c.send("{\"_cmd\": \"listen\", \"id\": 1, \"count\": -1, \"decimateFactor\": 1, "
			"\"start\": 0, \"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");
		CHECK(dev->connections.size() == 1);
		CHECK(dev->listeners.size() == 1);
	}
	CHECK(dev->connections.empty());
	CHECK(dev->listeners.empty());
}

TEST_CASE("messages for unknown sessions are ignored"){
	fake_session_ptr stray = std::make_shared<FakeSession>();
	ws_on_message(stray, "{\"_cmd\": \"selectDevice\", \"id\": \"x\"}");
	CHECK(stray->wsMessages.empty());
	ws_on_close(stray); // no-op
}

}
