// Tests for stream listeners (streaming_device/stream_listener.cpp):
// decimation, counts, start indexing, triggers, JSON and binary updates.

#include "support.hpp"
#include <cmath>
#include <cstring>
#include <sstream>

template <typename T>
static T rd(const std::string& buf, size_t off){
	T v;
	REQUIRE(off + sizeof(T) <= buf.size());
	memcpy(&v, buf.data() + off, sizeof(T));
	return v;
}

static listener_ptr listen(test_device_ptr dev, FakeClientConn& conn, const std::string& json){
	JSONNode n = jparse(json);
	listener_ptr l = makeStreamListener(dev.get(), &conn, n);
	dev->addListener(l);
	return l;
}

TEST_SUITE("stream_listener"){

TEST_CASE("decimation averages chunks and count limits output"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	listen(dev, conn, "{\"id\": 1, \"decimateFactor\": 2, \"start\": 0, \"count\": 3, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");

	dev->feedRamp(10);

	std::vector<JSONNode> ups = conn.actions("update");
	REQUIRE(ups.size() == 1);
	JSONNode& u = ups[0];
	CHECK(u.at("id").as_int() == 1);
	CHECK(u.at("idx").as_int() == 0);
	CHECK(u.at("sampleIndex").as_int() == 0);
	CHECK(u.at("done").as_bool() == true);

	JSONNode data = u.at("data");
	REQUIRE(data.size() == 1);
	JSONNode& s0 = data[0];
	REQUIRE(s0.size() == 3);
	CHECK(s0[0].as_float() == doctest::Approx(0.5));  // avg(0,1)
	CHECK(s0[1].as_float() == doctest::Approx(2.5));  // avg(2,3)
	CHECK(s0[2].as_float() == doctest::Approx(4.5));  // avg(4,5)

	// count reached; listener removed
	CHECK(dev->listeners.empty());
}

TEST_CASE("updates arrive incrementally; sampleIndex only on the first"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	listen(dev, conn, "{\"id\": 2, \"decimateFactor\": 1, \"start\": 0, \"count\": 4, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");

	dev->feedRamp(3); // only 2 chunks available (sample at capture_i-1 withheld)
	dev->feedRamp(3);

	std::vector<JSONNode> ups = conn.actions("update");
	REQUIRE(ups.size() == 2);
	CHECK(ups[0].at("idx").as_int() == 0);
	CHECK(jhas(ups[0], "sampleIndex"));
	CHECK(ups[1].at("idx").as_int() > 0);
	CHECK(!jhas(ups[1], "sampleIndex"));
	CHECK(ups[1].at("done").as_bool() == true);

	// total of `count` values across updates, in order
	std::vector<double> vals;
	for (size_t i = 0; i < ups.size(); i++){
		JSONNode d = ups[i].at("data")[0];
		for (unsigned j = 0; j < d.size(); j++) vals.push_back(d[j].as_float());
	}
	REQUIRE(vals.size() == 4);
	for (int k = 0; k < 4; k++) CHECK(vals[k] == doctest::Approx(k));
}

TEST_CASE("count -1 streams indefinitely (pixelpulse live mode)"){
	test_device_ptr dev = makeTestDevice(0.001, 100, true);
	FakeClientConn conn;
	listen(dev, conn, "{\"id\": 3, \"decimateFactor\": 1, \"start\": 0, \"count\": -1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");

	dev->feedRamp(10);
	dev->feedRamp(10);
	std::vector<JSONNode> ups = conn.actions("update");
	CHECK(ups.size() == 2);
	for (size_t i = 0; i < ups.size(); i++) CHECK(!jhas(ups[i], "done"));
	CHECK(dev->listeners.size() == 1);
}

TEST_CASE("start -1 begins at the latest sample"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	dev->feedRamp(5);
	listener_ptr l = listen(dev, conn,
		"{\"id\": 4, \"start\": -1, \"count\": 2, \"decimateFactor\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");
	CHECK(l->index == 5);

	dev->feedRamp(4);
	std::vector<JSONNode> ups = conn.actions("update");
	REQUIRE(!ups.empty());
	CHECK(ups[0].at("sampleIndex").as_int() == 5);
	CHECK(ups[0].at("data")[0][0].as_float() == doctest::Approx(5));
}

TEST_CASE("negative start indexes relative to latest; clamped to 0"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	dev->feedRamp(5);

	// inspect the initial index before addListener consumes available data
	JSONNode n1 = jparse("{\"id\": 5, \"start\": -3, \"count\": 1, \"decimateFactor\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");
	listener_ptr l1 = makeStreamListener(dev.get(), &conn, n1);
	CHECK(l1->index == 3);

	JSONNode n2 = jparse("{\"id\": 6, \"start\": -100, \"count\": 1, \"decimateFactor\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");
	listener_ptr l2 = makeStreamListener(dev.get(), &conn, n2);
	CHECK(l2->index == 0);
}

TEST_CASE("multiple streams keep request order in data"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	listen(dev, conn, "{\"id\": 7, \"decimateFactor\": 1, \"start\": 0, \"count\": 1, "
		"\"streams\": [{\"channel\": \"b\", \"stream\": \"i\"}, "
		               "{\"channel\": \"a\", \"stream\": \"v\"}]}");
	dev->feedRamp(2);

	std::vector<JSONNode> ups = conn.actions("update");
	REQUIRE(ups.size() == 1);
	JSONNode data = ups[0].at("data");
	REQUIRE(data.size() == 2);
	CHECK(data[0][0].as_float() == doctest::Approx(100)); // b_i at sample 0
	CHECK(data[1][0].as_float() == doctest::Approx(0));   // a_v at sample 0
}

TEST_CASE("decimateFactor 0 is coerced to 1"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	listener_ptr l = listen(dev, conn,
		"{\"id\": 8, \"decimateFactor\": 0, \"start\": 0, \"count\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");
	CHECK(l->decimateFactor == 1);
}

TEST_CASE("listener requires id and count"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	JSONNode noId = jparse("{\"count\": 1, \"streams\": []}");
	CHECK_THROWS_AS(makeStreamListener(dev.get(), &conn, noId), ErrorStringException);
	JSONNode noCount = jparse("{\"id\": 1, \"streams\": []}");
	CHECK_THROWS_AS(makeStreamListener(dev.get(), &conn, noCount), ErrorStringException);
}

TEST_CASE("unknown channel or stream in request throws"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	JSONNode badChan = jparse("{\"id\": 1, \"count\": 1, "
		"\"streams\": [{\"channel\": \"z\", \"stream\": \"v\"}]}");
	CHECK_THROWS_AS(makeStreamListener(dev.get(), &conn, badChan), ErrorStringException);
	JSONNode badStream = jparse("{\"id\": 1, \"count\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"z\"}]}");
	CHECK_THROWS_AS(makeStreamListener(dev.get(), &conn, badStream), ErrorStringException);
}

// --- Binary updates (pixelpulse's hot path) ---

TEST_CASE("binary update frame layout"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	listen(dev, conn, "{\"id\": 42, \"binary\": true, \"decimateFactor\": 1, "
		"\"start\": 0, \"count\": 2, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}, "
		               "{\"channel\": \"a\", \"stream\": \"i\"}]}");
	dev->feedRamp(4);

	CHECK(conn.actions("update").empty()); // no JSON updates
	REQUIRE(conn.binaryFrames.size() == 1);
	const std::string& f = conn.binaryFrames[0];

	REQUIRE(f.size() == 24 + 2 * 2 * 4); // header + 2 streams * 2 chunks * f32
	CHECK(rd<uint8_t>(f, 0) == 1);          // frame type
	uint8_t flags = rd<uint8_t>(f, 1);
	CHECK((flags & 1) == 1);                // done
	CHECK((flags & 2) == 0);                // not force-triggered
	CHECK((flags & 4) == 0);                // no trigger -> subsample invalid
	CHECK(rd<uint16_t>(f, 2) == 2);         // stream count
	CHECK(rd<uint32_t>(f, 4) == 42);        // listener id
	CHECK(rd<uint32_t>(f, 8) == 0);         // idx
	CHECK(rd<uint32_t>(f, 12) == 0);        // sampleIndex
	CHECK(rd<uint32_t>(f, 20) == 2);        // nchunks

	// stream-major payload: a_v samples then a_i samples
	CHECK(rd<float>(f, 24) == doctest::Approx(0));
	CHECK(rd<float>(f, 28) == doctest::Approx(1));
	CHECK(rd<float>(f, 32) == doctest::Approx(0));  // a_i = 10*k
	CHECK(rd<float>(f, 36) == doctest::Approx(10));

	CHECK(dev->listeners.empty()); // count reached
}

TEST_CASE("binary update transports NaN for overwritten samples"){
	test_device_ptr dev = makeTestDevice(0.001, 10, true);
	FakeClientConn conn;
	dev->feedRamp(25); // ring of 10 has wrapped; sample 0 long gone
	listen(dev, conn, "{\"id\": 9, \"binary\": true, \"decimateFactor\": 1, "
		"\"start\": 0, \"count\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");

	REQUIRE(conn.binaryFrames.size() == 1);
	float v = rd<float>(conn.binaryFrames[0], 24);
	CHECK(std::isnan(v));
}

// --- Triggers ---

TEST_CASE("in-stream trigger fires on rising edge through level"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	listen(dev, conn, "{\"id\": 10, \"decimateFactor\": 1, \"start\": 0, \"count\": 2, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}], "
		"\"trigger\": {\"type\": \"in\", \"channel\": \"a\", \"stream\": \"v\", "
		              "\"level\": 2.5, \"repeat\": false}}");

	dev->feedRamp(6); // 0,1,2,3,4,5 -> crossing 2.5 between samples 2 and 3

	std::vector<JSONNode> ups = conn.actions("update");
	REQUIRE(!ups.empty());
	CHECK(ups[0].at("sampleIndex").as_int() == 3);
	CHECK(jhas(ups[0], "subsample")); // triggered -> subsample reported (0 for in-stream)
	CHECK(ups[0].at("subsample").as_float() == doctest::Approx(0));
	CHECK(ups[0].at("data")[0][0].as_float() == doctest::Approx(3));
}

TEST_CASE("in-stream trigger offset shifts the window"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	listen(dev, conn, "{\"id\": 11, \"decimateFactor\": 1, \"start\": 0, \"count\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}], "
		"\"trigger\": {\"type\": \"in\", \"channel\": \"a\", \"stream\": \"v\", "
		              "\"level\": 2.5, \"offset\": 1, \"repeat\": false}}");
	dev->feedRamp(6);
	std::vector<JSONNode> ups = conn.actions("update");
	REQUIRE(!ups.empty());
	CHECK(ups[0].at("sampleIndex").as_int() == 4);
}

TEST_CASE("repeating trigger re-arms after each sweep"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	listen(dev, conn, "{\"id\": 12, \"decimateFactor\": 1, \"start\": 0, \"count\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}], "
		"\"trigger\": {\"type\": \"in\", \"channel\": \"a\", \"stream\": \"v\", "
		              "\"level\": 0.5, \"repeat\": true}}");

	// square-ish signal: 0,0,1,1 repeated. Rising edges at samples 2, 6, 10...
	for (int k = 0; k < 12; k++){
		float v = (k % 4 >= 2) ? 1.0f : 0.0f;
		dev->feed(v, 0, 0, 0);
	}
	dev->packetDone();

	std::vector<JSONNode> ups = conn.actions("update");
	REQUIRE(ups.size() >= 2);
	CHECK(ups[0].at("sampleIndex").as_int() == 2);
	CHECK(ups[1].at("sampleIndex").as_int() == 6);
	// repeating listeners are never done
	for (size_t i = 0; i < ups.size(); i++) CHECK(!jhas(ups[i], "done"));
	CHECK(dev->listeners.size() == 1);
}

TEST_CASE("trigger holdoff delays re-arm"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	listen(dev, conn, "{\"id\": 13, \"decimateFactor\": 1, \"start\": 0, \"count\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}], "
		"\"trigger\": {\"type\": \"in\", \"channel\": \"a\", \"stream\": \"v\", "
		              "\"level\": 0.5, \"repeat\": true, \"holdoff\": 3}}");

	for (int k = 0; k < 14; k++){
		float v = (k % 4 >= 2) ? 1.0f : 0.0f;
		dev->feed(v, 0, 0, 0);
	}
	dev->packetDone();

	std::vector<JSONNode> ups = conn.actions("update");
	REQUIRE(ups.size() >= 2);
	CHECK(ups[0].at("sampleIndex").as_int() == 2);
	// holdoff skips the edge at 6; next detectable rising edge is at 10
	CHECK(ups[1].at("sampleIndex").as_int() == 10);
}

TEST_CASE("forced trigger fires after `force` samples without an edge"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	listen(dev, conn, "{\"id\": 14, \"decimateFactor\": 1, \"start\": 0, \"count\": 2, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}], "
		"\"trigger\": {\"type\": \"in\", \"channel\": \"a\", \"stream\": \"v\", "
		              "\"level\": 5, \"repeat\": false, \"force\": 5}}");

	// flat signal, never crosses level 5
	for (int k = 0; k < 10; k++) dev->feed(1, 0, 0, 0);
	dev->packetDone();
	for (int k = 0; k < 4; k++) dev->feed(1, 0, 0, 0);
	dev->packetDone();

	std::vector<JSONNode> ups = conn.actions("update");
	REQUIRE(!ups.empty());
	CHECK(ups[0].at("triggerForced").as_bool() == true);
}

TEST_CASE("out-source trigger aligns to output phase zero"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	// square wave, period 10, phase 0 on channel a
	dev->setOutput(&dev->channel_a, makeSource(1, "square", 0, 1, 10, 0, false));
	dev->consumeOutput(35);

	listen(dev, conn, "{\"id\": 15, \"decimateFactor\": 1, \"start\": 0, \"count\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}], "
		"\"trigger\": {\"type\": \"out\", \"channel\": \"a\", \"repeat\": false}}");
	dev->feedRamp(30);

	std::vector<JSONNode> ups = conn.actions("update");
	REQUIRE(!ups.empty());
	CHECK(ups[0].at("sampleIndex").as_int() == 10);
	CHECK(ups[0].at("subsample").as_float() == doctest::Approx(0));
}

TEST_CASE("out-source trigger reports fractional subsample error"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	// sine with non-integer period: phase zero at sample 7.25
	dev->setOutput(&dev->channel_a, makeSource(1, "sine", 0, 1, 7.5, 0.25, false));
	dev->consumeOutput(20);

	listen(dev, conn, "{\"id\": 16, \"decimateFactor\": 1, \"start\": 0, \"count\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}], "
		"\"trigger\": {\"type\": \"out\", \"channel\": \"a\", \"repeat\": false}}");
	dev->feedRamp(15);

	std::vector<JSONNode> ups = conn.actions("update");
	REQUIRE(!ups.empty());
	CHECK(ups[0].at("sampleIndex").as_int() == 7);
	CHECK(ups[0].at("subsample").as_float() == doctest::Approx(0.25).epsilon(0.01));
}

TEST_CASE("large negative trigger offset is bounded by holdoff"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	JSONNode n = jparse("{\"id\": 17, \"decimateFactor\": 1, \"start\": 0, \"count\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}], "
		"\"trigger\": {\"type\": \"in\", \"channel\": \"a\", \"stream\": \"v\", "
		              "\"level\": 1, \"offset\": -10, \"holdoff\": 2}}");
	listener_ptr l = makeStreamListener(dev.get(), &conn, n);
	CHECK(l->triggerHoldoff == 10);
}

TEST_CASE("invalid trigger configuration throws"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn conn;
	JSONNode badType = jparse("{\"id\": 1, \"count\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}], "
		"\"trigger\": {\"type\": \"nope\", \"channel\": \"a\"}}");
	CHECK_THROWS_AS(makeStreamListener(dev.get(), &conn, badType), ErrorStringException);

	JSONNode badChan = jparse("{\"id\": 1, \"count\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}], "
		"\"trigger\": {\"type\": \"out\", \"channel\": \"z\"}}");
	CHECK_THROWS_AS(makeStreamListener(dev.get(), &conn, badChan), ErrorStringException);

	JSONNode inMissingLevel = jparse("{\"id\": 1, \"count\": 1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}], "
		"\"trigger\": {\"type\": \"in\", \"channel\": \"a\", \"stream\": \"v\"}}");
	CHECK_THROWS_AS(makeStreamListener(dev.get(), &conn, inMissingLevel), ErrorStringException);
}

TEST_CASE("capture reset rewinds listeners"){
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn;
	listener_ptr l = listen(dev, conn,
		"{\"id\": 18, \"decimateFactor\": 1, \"start\": 0, \"count\": -1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");
	dev->feedRamp(10);
	CHECK(l->index > 0);

	dev->reset_capture();
	CHECK(l->index == 0);
	CHECK(l->outIndex == 0);
}

TEST_CASE("closed client removes REST-style listener on next data"){
	// WSStreamListener holds a raw ClientConn*; verify that listener removal on
	// client detach is handled by onClientDetach instead
	test_device_ptr dev = makeTestDevice(0.001, 100);
	FakeClientConn conn1, conn2;
	listen(dev, conn1, "{\"id\": 1, \"decimateFactor\": 1, \"start\": 0, \"count\": -1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");
	listen(dev, conn2, "{\"id\": 1, \"decimateFactor\": 1, \"start\": 0, \"count\": -1, "
		"\"streams\": [{\"channel\": \"a\", \"stream\": \"v\"}]}");
	CHECK(dev->listeners.size() == 2);

	dev->onClientDetach(&conn1);
	CHECK(dev->listeners.size() == 1);
	// remaining listener belongs to conn2
	CHECK(dev->findListener(&conn2, 1) != listener_ptr());
	CHECK(dev->findListener(&conn1, 1) == listener_ptr());
}

}
