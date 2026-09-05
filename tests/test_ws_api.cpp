// Tests for StreamingDevice::processMessage (streaming_device/ws_api.cpp) —
// the WebSocket command set shared by all streaming devices.

#include "support.hpp"

static bool cmd(test_device_ptr dev, FakeClientConn& conn, const std::string& json){
	JSONNode n = jparse(json);
	string c = n.at("_cmd").as_string();
	return dev->processMessage(conn, c, n);
}

TEST_SUITE("ws_api"){

TEST_CASE("onClientAttach sends deviceConfig"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	conn.selectDevice(dev);
	std::vector<JSONNode> dc = conn.actions("deviceConfig");
	REQUIRE(dc.size() == 1);
	JSONNode d = dc[0].at("device");
	CHECK(d.at("id").as_string() == "com.nonolithlabs.test~TEST001");
	CHECK(jhas(d, "channels"));
}

TEST_CASE("configure command applies settings and broadcasts"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	conn.selectDevice(dev);
	conn.clear();

	CHECK(cmd(dev, conn, "{\"_cmd\": \"configure\", \"mode\": 0, "
		"\"sampleTime\": 0.0005, \"samples\": 42, \"continuous\": true}"));
	CHECK(dev->captureSamples == 42);
	CHECK(dev->captureContinuous == true);
	CHECK(dev->rawMode == false); // default
	CHECK(dev->sampleTime == doctest::Approx(0.0005));
	CHECK(conn.actions("deviceConfig").size() == 1);
}

TEST_CASE("configure requires mode, samples, sampleTime"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	CHECK_THROWS_AS(cmd(dev, conn, "{\"_cmd\": \"configure\", \"samples\": 10}"),
	                ErrorStringException);
}

TEST_CASE("startCapture and pauseCapture commands"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	conn.selectDevice(dev);
	conn.clear();

	CHECK(cmd(dev, conn, "{\"_cmd\": \"startCapture\"}"));
	CHECK(dev->captureState == true);
	CHECK(cmd(dev, conn, "{\"_cmd\": \"pauseCapture\"}"));
	CHECK(dev->captureState == false);

	std::vector<JSONNode> cs = conn.actions("captureState");
	REQUIRE(cs.size() == 2);
	CHECK(cs[0].at("state").as_bool() == true);
	CHECK(cs[1].at("state").as_bool() == false);
}

TEST_CASE("set command replaces a channel output source"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	conn.selectDevice(dev);
	conn.clear();

	CHECK(cmd(dev, conn, "{\"_cmd\": \"set\", \"channel\": \"a\", \"mode\": 1, "
		"\"source\": \"sine\", \"offset\": 2.5, \"amplitude\": 1, \"period\": 100}"));

	std::vector<JSONNode> oc = conn.actions("outputChanged");
	REQUIRE(oc.size() == 1);
	CHECK(oc[0].at("channel").as_string() == "a");
	CHECK(oc[0].at("source").as_string() == "sine");
	CHECK(oc[0].at("offset").as_float() == doctest::Approx(2.5));
	CHECK(oc[0].at("mode").as_int() == 1);
	CHECK(dev->channel_a.source->displayName() == "sine");
}

TEST_CASE("set command on unknown channel throws"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	CHECK_THROWS_AS(cmd(dev, conn, "{\"_cmd\": \"set\", \"channel\": \"z\", "
		"\"mode\": 1, \"source\": \"constant\", \"value\": 0}"),
		ErrorStringException);
}

TEST_CASE("setGain command"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	conn.selectDevice(dev);
	conn.clear();

	CHECK(cmd(dev, conn, "{\"_cmd\": \"setGain\", \"channel\": \"a\", "
		"\"stream\": \"v\", \"gain\": 4}"));
	CHECK(dev->a_v.getGain() == doctest::Approx(4));
	CHECK(conn.actions("gainChanged").size() == 1);

	CHECK_THROWS_AS(cmd(dev, conn, "{\"_cmd\": \"setGain\", \"channel\": \"z\", "
		"\"stream\": \"v\"}"), ErrorStringException);
	CHECK_THROWS_AS(cmd(dev, conn, "{\"_cmd\": \"setGain\", \"channel\": \"a\", "
		"\"stream\": \"z\"}"), ErrorStringException);
}

TEST_CASE("setCurrentLimit command"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	CHECK(cmd(dev, conn, "{\"_cmd\": \"setCurrentLimit\", \"currentLimit\": 200}"));
	CHECK(dev->lastCurrentLimit == 200);
}

TEST_CASE("listen and cancelListen manage listeners per client"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	conn.selectDevice(dev);

	CHECK(cmd(dev, conn, "{\"_cmd\": \"listen\", \"id\": 1, \"count\": -1, "
		"\"decimateFactor\": 1, \"start\": 0, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}"));
	CHECK(dev->listeners.size() == 1);

	// same id replaces, not duplicates
	CHECK(cmd(dev, conn, "{\"_cmd\": \"listen\", \"id\": 1, \"count\": -1, "
		"\"decimateFactor\": 2, \"start\": 0, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}"));
	CHECK(dev->listeners.size() == 1);
	CHECK((*dev->listeners.begin())->decimateFactor == 2);

	// different id adds
	CHECK(cmd(dev, conn, "{\"_cmd\": \"listen\", \"id\": 2, \"count\": -1, "
		"\"decimateFactor\": 1, \"start\": 0, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}"));
	CHECK(dev->listeners.size() == 2);

	CHECK(cmd(dev, conn, "{\"_cmd\": \"cancelListen\", \"id\": 1}"));
	CHECK(dev->listeners.size() == 1);

	// cancelling an unknown id is a no-op
	CHECK(cmd(dev, conn, "{\"_cmd\": \"cancelListen\", \"id\": 99}"));
	CHECK(dev->listeners.size() == 1);
}

TEST_CASE("configure clears active listeners"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	conn.selectDevice(dev);
	cmd(dev, conn, "{\"_cmd\": \"listen\", \"id\": 1, \"count\": -1, "
		"\"decimateFactor\": 1, \"start\": 0, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");
	CHECK(dev->listeners.size() == 1);

	cmd(dev, conn, "{\"_cmd\": \"configure\", \"mode\": 0, "
		"\"sampleTime\": 0.001, \"samples\": 10}");
	CHECK(dev->listeners.empty());
}

TEST_CASE("unknown command returns false"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	CHECK(!cmd(dev, conn, "{\"_cmd\": \"fooBar\"}"));
}

TEST_CASE("onClientDetach removes only that client's listeners"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn a, b;
	a.selectDevice(dev);
	b.selectDevice(dev);
	cmd(dev, a, "{\"_cmd\": \"listen\", \"id\": 1, \"count\": -1, "
		"\"decimateFactor\": 1, \"start\": 0, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");
	cmd(dev, b, "{\"_cmd\": \"listen\", \"id\": 1, \"count\": -1, "
		"\"decimateFactor\": 1, \"start\": 0, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");
	CHECK(dev->listeners.size() == 2);

	dev->onClientDetach(&b);
	b.device.reset(); // prevent double-detach in the destructor
	CHECK(dev->listeners.size() == 1);
	CHECK(dev->findListener(&a, 1) != listener_ptr());
}

}
