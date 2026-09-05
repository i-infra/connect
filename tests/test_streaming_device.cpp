// Tests for the StreamingDevice base class: sample buffer, capture
// lifecycle, JSON serialization, channel/stream lookup.

#include "support.hpp"
#include <cmath>

TEST_SUITE("streaming_device"){

TEST_CASE("put/get roundtrip and not-yet-collected NaN"){
	test_device_ptr dev = makeTestDevice(0.001, 10);
	CHECK(std::isnan(dev->get(dev->a_v, 0))); // nothing collected yet

	dev->feed(1.5, 2.5, 3.5, 4.5);
	CHECK(dev->get(dev->a_v, 0) == doctest::Approx(1.5));
	CHECK(dev->get(dev->a_i, 0) == doctest::Approx(2.5));
	CHECK(dev->get(dev->b_v, 0) == doctest::Approx(3.5));
	CHECK(dev->get(dev->b_i, 0) == doctest::Approx(4.5));

	// index 1 not collected yet
	CHECK(std::isnan(dev->get(dev->a_v, 1)));
}

TEST_CASE("non-continuous capture stops storing at captureSamples"){
	test_device_ptr dev = makeTestDevice(0.001, 5);
	for (int k = 0; k < 8; k++) dev->feed(k, 0, 0, 0);
	// capture_i keeps counting but only 5 samples are stored
	CHECK(dev->capture_i == 8);
	CHECK(dev->get(dev->a_v, 4) == doctest::Approx(4));
	// samples 5..7 were dropped; get() wraps around and reads the stale
	// stored value at i % captureSamples (documented quirk of the
	// non-continuous overshoot window)
	CHECK(dev->get(dev->a_v, 5) == doctest::Approx(0));
	// indexes at/after capture_i are NaN
	CHECK(std::isnan(dev->get(dev->a_v, 8)));
}

TEST_CASE("continuous ring buffer overwrites oldest samples"){
	test_device_ptr dev = makeTestDevice(0.001, 10, true);
	for (int k = 0; k < 25; k++) dev->feed(k, 0, 0, 0);
	CHECK(dev->buffer_max() == 25);
	CHECK(dev->buffer_min() == 15);
	// overwritten samples read NaN (boundary sample buffer_min() included)
	CHECK(std::isnan(dev->get(dev->a_v, 0)));
	CHECK(std::isnan(dev->get(dev->a_v, 15)));
	CHECK(dev->get(dev->a_v, 16) == doctest::Approx(16));
	CHECK(dev->get(dev->a_v, 24) == doctest::Approx(24));
	CHECK(std::isnan(dev->get(dev->a_v, 25))); // not collected
}

TEST_CASE("buffer_min is 0 before the ring wraps"){
	test_device_ptr dev = makeTestDevice(0.001, 10, true);
	for (int k = 0; k < 7; k++) dev->feed(k, 0, 0, 0);
	CHECK(dev->buffer_min() == 0);
	CHECK(dev->buffer_max() == 7);
}

TEST_CASE("resample averages a chunk"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	for (int k = 0; k < 10; k++) dev->feed(k, 0, 0, 0);
	CHECK(dev->resample(dev->a_v, 0, 4) == doctest::Approx((0+1+2+3)/4.0));
	CHECK(dev->resample(dev->a_v, 4, 2) == doctest::Approx(4.5));
	// chunk extending past collected data is NaN
	CHECK(std::isnan(dev->resample(dev->a_v, 8, 4)));
	// exact fit at the end is allowed
	CHECK(dev->resample(dev->a_v, 8, 2) == doctest::Approx(8.5));
}

TEST_CASE("capture lifecycle: start, pause, and notifications"){
	test_device_ptr dev = makeTestDevice(0.001, 10);
	FakeClientConn conn;
	conn.selectDevice(dev);
	CHECK(conn.actions("deviceConfig").size() == 1); // sent on attach
	conn.clear();

	dev->start_capture();
	CHECK(dev->captureState == true);
	CHECK(dev->starts == 1);
	{
		std::vector<JSONNode> cs = conn.actions("captureState");
		REQUIRE(cs.size() == 1);
		CHECK(cs[0].at("state").as_bool() == true);
		CHECK(cs[0].at("done").as_bool() == false);
	}

	// starting again is a no-op
	dev->start_capture();
	CHECK(dev->starts == 1);

	conn.clear();
	dev->pause_capture();
	CHECK(dev->captureState == false);
	CHECK(dev->pauses >= 1);
	{
		std::vector<JSONNode> cs = conn.actions("captureState");
		REQUIRE(cs.size() == 1);
		CHECK(cs[0].at("state").as_bool() == false);
	}

	// pausing again is a no-op
	int pausesBefore = dev->pauses;
	dev->pause_capture();
	CHECK(dev->pauses == pausesBefore);
}

TEST_CASE("non-continuous capture completes when buffer is full"){
	test_device_ptr dev = makeTestDevice(0.001, 5);
	FakeClientConn conn;
	conn.selectDevice(dev);
	dev->start_capture();
	conn.clear();

	dev->feedRamp(5);
	CHECK(dev->captureDone == true);
	CHECK(dev->captureState == false);
	std::vector<JSONNode> cs = conn.actions("captureState");
	REQUIRE(!cs.empty());
	CHECK(cs.back().at("done").as_bool() == true);
	CHECK(cs.back().at("state").as_bool() == false);

	// restarting after done resets the capture
	conn.clear();
	dev->start_capture();
	CHECK(dev->captureDone == false);
	CHECK(dev->capture_i == 0);
	CHECK(dev->resets == 1);
	CHECK(conn.actions("captureReset").size() == 1);
}

TEST_CASE("reset_capture zeroes counters and notifies"){
	test_device_ptr dev = makeTestDevice(0.001, 10);
	FakeClientConn conn;
	conn.selectDevice(dev);
	dev->feedRamp(3);
	conn.clear();

	dev->reset_capture();
	CHECK(dev->capture_i == 0);
	CHECK(dev->capture_o == 0);
	CHECK(conn.actions("captureReset").size() == 1);
}

TEST_CASE("channel and stream lookup"){
	test_device_ptr dev = makeTestDevice();
	CHECK(dev->channelById("a") == &dev->channel_a);
	CHECK(dev->channelById("b") == &dev->channel_b);
	CHECK(dev->channelById("c") == (Channel*) 0);

	CHECK(dev->channel_a.streamById("v") == &dev->a_v);
	CHECK(dev->channel_a.streamById("x") == (Stream*) 0);

	CHECK(dev->findStream("a", "i") == &dev->a_i);
	CHECK_THROWS_AS(dev->findStream("nope", "v"), ErrorStringException);
	CHECK_THROWS_AS(dev->findStream("a", "nope"), ErrorStringException);
}

TEST_CASE("stateToJSON full state"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	JSONNode n = dev->stateToJSON();
	CHECK(n.at("id").as_string() == "com.nonolithlabs.test~TEST001");
	CHECK(n.at("model").as_string() == "com.nonolithlabs.test");
	CHECK(n.at("hwVersion").as_string() == "T1");
	CHECK(n.at("fwVersion").as_string() == "9.9");
	CHECK(n.at("serial").as_string() == "TEST001");
	CHECK(n.at("sampleTime").as_float() == doctest::Approx(0.001));
	CHECK(n.at("minSampleTime").as_float() == doctest::Approx(0.00001));
	CHECK(n.at("mode").as_int() == 0);
	CHECK(n.at("samples").as_int() == 100);
	CHECK(n.at("length").as_float() == doctest::Approx(0.1));
	CHECK(n.at("continuous").as_bool() == false);
	CHECK(n.at("raw").as_bool() == false);
	CHECK(n.at("captureState").as_bool() == false);
	CHECK(n.at("captureDone").as_bool() == false);

	JSONNode channels = n.at("channels");
	JSONNode a = channels.at("a");
	CHECK(a.at("id").as_string() == "a");
	CHECK(a.at("displayName").as_string() == "A");
	JSONNode av = a.at("streams").at("v");
	CHECK(av.at("units").as_string() == "V");
	CHECK(av.at("min").as_float() == doctest::Approx(-5));
	CHECK(av.at("max").as_float() == doctest::Approx(5));
	CHECK(av.at("outputMode").as_int() == 1);
	CHECK(av.at("gain").as_float() == doctest::Approx(1));
	// output source description present
	JSONNode out = a.at("output");
	CHECK(out.at("source").as_string() == "constant");
}

TEST_CASE("stateToJSON configOnly omits channels and capture state"){
	test_device_ptr dev = makeTestDevice();
	JSONNode n = dev->stateToJSON(true);
	CHECK(jhas(n, "sampleTime"));
	CHECK(jhas(n, "samples"));
	CHECK(!jhas(n, "channels"));
	CHECK(!jhas(n, "captureState"));
	CHECK(!jhas(n, "id"));
}

TEST_CASE("setOutput replaces source, stamps startSample, and notifies"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	conn.selectDevice(dev);
	dev->consumeOutput(7);
	conn.clear();

	dev->setOutput(&dev->channel_a, makeConstantSource(1, 2.5));
	CHECK(dev->channel_a.source->startSample == 7);
	CHECK(dev->channel_a.source->mode == 1);

	std::vector<JSONNode> oc = conn.actions("outputChanged");
	REQUIRE(oc.size() == 1);
	CHECK(oc[0].at("channel").as_string() == "a");
	CHECK(oc[0].at("source").as_string() == "constant");
	CHECK(oc[0].at("value").as_float() == doctest::Approx(2.5));
	CHECK(oc[0].at("effective").as_bool() == false);
}

TEST_CASE("setGain scales by normalGain and notifies"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	conn.selectDevice(dev);
	conn.clear();

	dev->setGain(&dev->channel_a, &dev->a_v, 2.0);
	CHECK(dev->lastInternalGain == 2);
	CHECK(dev->a_v.getGain() == doctest::Approx(2.0));

	std::vector<JSONNode> gc = conn.actions("gainChanged");
	REQUIRE(gc.size() == 1);
	CHECK(gc[0].at("channel").as_string() == "a");
	CHECK(gc[0].at("stream").as_string() == "v");
	CHECK(gc[0].at("gain").as_float() == doctest::Approx(2.0));
}

TEST_CASE("Stream::allocate replaces the buffer"){
	Stream s("x", "X", "V", 0, 5);
	CHECK(s.data == (float*) 0);
	CHECK(s.allocate(16));
	CHECK(s.data != (float*) 0);
	s.data[0] = 42;
	CHECK(s.allocate(32)); // reallocation frees the old buffer
	CHECK(s.data != (float*) 0);
}

TEST_CASE("configure resets capture counters and broadcasts deviceConfig"){
	test_device_ptr dev = makeTestDevice(0.001, 10);
	FakeClientConn conn;
	conn.selectDevice(dev);
	dev->feedRamp(5);
	conn.clear();

	dev->configure(0, 0.0005, 20, true, false);
	CHECK(dev->capture_i == 0);
	CHECK(dev->captureSamples == 20);
	CHECK(dev->captureContinuous == true);
	CHECK(dev->sampleTime == doctest::Approx(0.0005));
	CHECK(dev->captureLength == doctest::Approx(0.01));

	std::vector<JSONNode> dc = conn.actions("deviceConfig");
	REQUIRE(dc.size() == 1);
	CHECK(dc[0].at("device").at("samples").as_int() == 20);
	CHECK(dc[0].at("device").at("continuous").as_bool() == true);
}

TEST_CASE("onDisconnect broadcasts deviceDisconnected and clears listeners"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	conn.selectDevice(dev);
	conn.clear();

	JSONNode listen = jparse("{\"id\": 1, \"count\": 0, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");
	dev->addListener(makeStreamListener(dev.get(), &conn, listen));
	CHECK(dev->listeners.size() == 1);

	dev->onDisconnect();
	CHECK(dev->listeners.empty());
	std::vector<JSONNode> dd = conn.actions("deviceDisconnected");
	CHECK(dd.size() == 1);

	conn.device.reset(); // device already "gone"; skip detach in dtor
}

}
