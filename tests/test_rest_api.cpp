// Tests for the per-device REST API (streaming_device/rest_api.cpp),
// exercised through StreamingDevice::handleREST with a fake session.

#include "support.hpp"
#include <cmath>

TEST_SUITE("rest_api"){

TEST_CASE("GET / returns full device state"){
	test_device_ptr dev = makeTestDevice();
	fake_session_ptr r = restRequest(dev, "/");
	CHECK(r->status == 200);
	JSONNode n = r->jsonBody();
	CHECK(n.at("id").as_string() == "com.nonolithlabs.test~TEST001");
	CHECK(n.at("serial").as_string() == "TEST001");
	CHECK(jhas(n, "channels"));
	CHECK(jhas(n, "captureState"));
}

TEST_CASE("POST / controls capture state"){
	test_device_ptr dev = makeTestDevice();

	const char* onValues[] = {"capture=true", "capture=on", "capture=1"};
	const char* offValues[] = {"capture=false", "capture=off", "capture=0"};

	for (int i = 0; i < 3; i++){
		fake_session_ptr r = restRequest(dev, "/", "POST", onValues[i]);
		CHECK(r->status == 200);
		CHECK(dev->captureState == true);
		CHECK(r->jsonBody().at("captureState").as_bool() == true);

		fake_session_ptr r2 = restRequest(dev, "/", "POST", offValues[i]);
		CHECK(dev->captureState == false);
		CHECK(r2->jsonBody().at("captureState").as_bool() == false);
	}
}

TEST_CASE("POST / with no capture parameter leaves state alone"){
	test_device_ptr dev = makeTestDevice();
	fake_session_ptr r = restRequest(dev, "/", "POST", "");
	CHECK(r->status == 200);
	CHECK(dev->captureState == false);
}

TEST_CASE("GET /configuration returns config-only view"){
	test_device_ptr dev = makeTestDevice();
	fake_session_ptr r = restRequest(dev, "/configuration");
	CHECK(r->status == 200);
	JSONNode n = r->jsonBody();
	CHECK(jhas(n, "sampleTime"));
	CHECK(!jhas(n, "channels"));
	CHECK(!jhas(n, "captureState"));
}

TEST_CASE("POST /configuration applies samples and clamps sampleTime"){
	test_device_ptr dev = makeTestDevice(0.001, 100);

	fake_session_ptr r = restRequest(dev, "/configuration", "POST",
	                                 "samples=50&sampleTime=0.0005");
	CHECK(r->status == 200);
	CHECK(dev->captureSamples == 50);
	CHECK(dev->sampleTime == doctest::Approx(0.0005));

	// sampleTime above 1ms is clamped to 1ms
	restRequest(dev, "/configuration", "POST", "sampleTime=0.5");
	CHECK(dev->sampleTime == doctest::Approx(0.001));

	// non-positive sampleTime keeps the current value
	restRequest(dev, "/configuration", "POST", "sampleTime=-1");
	CHECK(dev->sampleTime == doctest::Approx(0.001));

	// reconfiguration clears captured data
	dev->feedRamp(10);
	restRequest(dev, "/configuration", "POST", "samples=30");
	CHECK(dev->capture_i == 0);
}

TEST_CASE("POST /configuration passes currentLimit through"){
	test_device_ptr dev = makeTestDevice();
	restRequest(dev, "/configuration", "POST", "currentLimit=150");
	CHECK(dev->lastCurrentLimit == 150);
}

TEST_CASE("GET /<channel> returns channel description"){
	test_device_ptr dev = makeTestDevice();
	fake_session_ptr r = restRequest(dev, "/a");
	CHECK(r->status == 200);
	JSONNode n = r->jsonBody();
	CHECK(n.at("id").as_string() == "a");
	CHECK(n.at("displayName").as_string() == "A");
	CHECK(jhas(n, "streams"));
	CHECK(jhas(n, "output"));
}

TEST_CASE("unknown channel or subresource is unhandled (404)"){
	test_device_ptr dev = makeTestDevice();
	CHECK(restRequest(dev, "/z")->status == 404);
	CHECK(restRequest(dev, "/a/bogus")->status == 404);
}

TEST_CASE("GET /<channel>/output describes the current source"){
	test_device_ptr dev = makeTestDevice();
	fake_session_ptr r = restRequest(dev, "/a/output");
	CHECK(r->status == 200);
	JSONNode n = r->jsonBody();
	CHECK(n.at("source").as_string() == "constant");
	CHECK(jhas(n, "mode"));
	CHECK(jhas(n, "startSample"));
	CHECK(jhas(n, "effective"));
}

TEST_CASE("POST /<channel>/output form: constant with mode aliases"){
	test_device_ptr dev = makeTestDevice();

	struct { const char* alias; int mode; } cases[] = {
		{"0", 0}, {"disabled", 0}, {"d", 0},
		{"1", 1}, {"svmi", 1}, {"v", 1}, {"SVMI", 1},
		{"2", 2}, {"simv", 2}, {"i", 2},
	};
	for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++){
		std::string post = std::string("mode=") + cases[i].alias + "&value=2.5";
		fake_session_ptr r = restRequest(dev, "/a/output", "POST", post);
		CHECK(r->status == 200);
		JSONNode n = r->jsonBody();
		CHECK(n.at("mode").as_int() == cases[i].mode);
		CHECK(n.at("source").as_string() == "constant");
		CHECK(n.at("value").as_float() == doctest::Approx(2.5));
	}
}

TEST_CASE("POST /<channel>/output form: sine frequency converts to period"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	fake_session_ptr r = restRequest(dev, "/a/output", "POST",
		"mode=v&wave=sine&value=1&amplitude=2&frequency=100");
	CHECK(r->status == 200);
	JSONNode n = r->jsonBody();
	CHECK(n.at("source").as_string() == "sine");
	// period = 1 / sampleTime / frequency = 1/0.001/100 = 10 samples
	CHECK(n.at("period").as_float() == doctest::Approx(10));
	CHECK(n.at("offset").as_float() == doctest::Approx(1));
	CHECK(n.at("amplitude").as_float() == doctest::Approx(2));
	CHECK(dev->channel_a.source->displayName() == "sine");
}

TEST_CASE("POST /<channel>/output form: non-positive frequency is coerced"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	fake_session_ptr r = restRequest(dev, "/a/output", "POST",
		"mode=v&wave=square&amplitude=1&frequency=0");
	CHECK(r->status == 200);
	// freq forced to 0.001 Hz -> period = 1/0.001/0.001 = 1e6 samples
	CHECK(r->jsonBody().at("period").as_float() == doctest::Approx(1e6));
}

TEST_CASE("POST /<channel>/output form: adv_square times convert to samples"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	fake_session_ptr r = restRequest(dev, "/a/output", "POST",
		"mode=1&wave=adv_square&value1=5&value2=-1&time1=0.004&time2=0.006");
	CHECK(r->status == 200);
	JSONNode n = r->jsonBody();
	CHECK(n.at("source").as_string() == "adv_square");
	CHECK(n.at("high").as_float() == doctest::Approx(5));
	CHECK(n.at("low").as_float() == doctest::Approx(-1));
	CHECK(n.at("highSamples").as_int() == 4);
	CHECK(n.at("lowSamples").as_int() == 6);
}

TEST_CASE("POST /<channel>/output form: adv_square zero times become one sample"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	fake_session_ptr r = restRequest(dev, "/a/output", "POST",
		"mode=1&wave=adv_square&value1=1&value2=0&time1=0&time2=0");
	CHECK(r->status == 200);
	JSONNode n = r->jsonBody();
	CHECK(n.at("highSamples").as_int() == 1);
	CHECK(n.at("lowSamples").as_int() == 1);
}

TEST_CASE("POST /<channel>/output form: arb points parse (plain and URL-escaped)"){
	test_device_ptr dev = makeTestDevice(0.001, 100);

	fake_session_ptr r = restRequest(dev, "/a/output", "POST",
		"mode=1&wave=arb&points=0:1,0.01:2");
	CHECK(r->status == 200);
	JSONNode n = r->jsonBody();
	CHECK(n.at("source").as_string() == "arb");
	JSONNode vals = n.at("values");
	REQUIRE(vals.size() == 2);
	CHECK(vals[0].at("t").as_int() == 0);
	CHECK(vals[0].at("v").as_float() == doctest::Approx(1));
	CHECK(vals[1].at("t").as_int() == 10); // 0.01s / 0.001s per sample
	CHECK(vals[1].at("v").as_float() == doctest::Approx(2));

	fake_session_ptr r2 = restRequest(dev, "/a/output", "POST",
		"mode=1&wave=arb&points=0%3A1%2C0.01%3A2");
	CHECK(r2->status == 200);
	CHECK(r2->jsonBody().at("values").size() == 2);
}

TEST_CASE("POST /<channel>/output form: invalid arb points give error 402"){
	test_device_ptr dev = makeTestDevice();
	fake_session_ptr r = restRequest(dev, "/a/output", "POST",
		"mode=1&wave=arb&points=garbage");
	CHECK(r->status == 402);
	JSONNode n = r->jsonBody();
	CHECK(jhas(n, "error"));
}

TEST_CASE("POST /<channel>/output JSON body"){
	test_device_ptr dev = makeTestDevice();
	fake_session_ptr r = restRequest(dev, "/a/output", "POST",
		"{\"source\": \"constant\", \"mode\": 2, \"value\": -0.05}");
	CHECK(r->status == 200);
	JSONNode n = r->jsonBody();
	CHECK(n.at("mode").as_int() == 2);
	CHECK(n.at("value").as_float() == doctest::Approx(-0.05));
}

TEST_CASE("POST /<channel>/output JSON body errors give 402"){
	test_device_ptr dev = makeTestDevice();
	fake_session_ptr r = restRequest(dev, "/a/output", "POST",
		"{\"source\": \"sawtooth\", \"mode\": 1}");
	CHECK(r->status == 402);
	JSONNode n = r->jsonBody();
	CHECK(jhas(n, "error"));
}

TEST_CASE("POST /<channel>/output hint is echoed"){
	test_device_ptr dev = makeTestDevice();
	fake_session_ptr r = restRequest(dev, "/a/output", "POST",
		"mode=1&value=1&hint=dutycycle:40");
	CHECK(r->status == 200);
	CHECK(r->jsonBody().at("hint").as_string() == "dutycycle:40");
}

TEST_CASE("POST /<channel>/input sets stream gains"){
	test_device_ptr dev = makeTestDevice();
	fake_session_ptr r = restRequest(dev, "/a/input", "POST", "gain_v=2");
	CHECK(r->status == 200);
	JSONNode g = r->jsonBody().at("gain");
	CHECK(g.at("v").as_float() == doctest::Approx(2));
	CHECK(g.at("i").as_float() == doctest::Approx(1));
	CHECK(dev->a_v.getGain() == doctest::Approx(2));

	fake_session_ptr r2 = restRequest(dev, "/a/input", "POST", "gain_v=1&gain_i=4");
	JSONNode g2 = r2->jsonBody().at("gain");
	CHECK(g2.at("v").as_float() == doctest::Approx(1));
	CHECK(g2.at("i").as_float() == doctest::Approx(4));
}

TEST_CASE("GET /<channel>/input streams CSV with header"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	fake_session_ptr r = restRequest(dev, "/a/input?resample=0.002&start=0&count=2");
	CHECK(r->status == 200);
	CHECK(r->body == "Voltage A (V),Current A (mA)\n");
	CHECK(!r->doneFlag); // long-lived response awaiting data

	dev->feedRamp(5);
	// two rows of averaged pairs, then the response completes
	CHECK(r->body == "Voltage A (V),Current A (mA)\n0.5, 5\n2.5, 25\n");
	CHECK(r->doneFlag);
	CHECK(dev->listeners.empty());
}

TEST_CASE("GET /<channel>/input header=0 omits the header row"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	fake_session_ptr r = restRequest(dev, "/a/input?resample=0.001&start=0&count=1&header=0");
	dev->feedRamp(3);
	CHECK(r->body == "0, 0\n");
}

TEST_CASE("GET /<channel>/input data arrives incrementally"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	fake_session_ptr r = restRequest(dev, "/a/input?resample=0.001&start=0&count=4&header=0");
	dev->feedRamp(3);
	CHECK(r->body == "0, 0\n1, 10\n2, 20\n");
	CHECK(!r->doneFlag);
	dev->feedRamp(3);
	CHECK(r->body == "0, 0\n1, 10\n2, 20\n3, 30\n");
	CHECK(r->doneFlag);
}

TEST_CASE("GET /<channel>/input stops when the client disconnects"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	// count=0 means unlimited for the REST listener
	fake_session_ptr r = restRequest(dev, "/a/input?resample=0.001&start=0&count=0&header=0");
	CHECK(dev->listeners.size() == 1);
	r->closed = true;
	dev->feedRamp(3);
	CHECK(dev->listeners.empty());
}

}
