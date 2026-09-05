// Tests for signal generation sources (streaming_device/output_source.cpp)

#include "support.hpp"
#include <cmath>
#include <memory>

typedef std::unique_ptr<OutputSource> source_ptr;

static JSONNode describe(OutputSource* s){
	JSONNode n(JSON_NODE);
	s->describeJSON(n);
	return n;
}

TEST_SUITE("output_source"){

TEST_CASE("constant source value and metadata"){
	source_ptr s(makeConstantSource(1, 2.5));
	CHECK(s->mode == 1);
	CHECK(s->displayName() == "constant");
	for (unsigned i = 0; i < 100; i += 10){
		CHECK(s->getValue(i, 0.001) == doctest::Approx(2.5));
	}
	// a constant is always at "phase zero"
	CHECK(s->getPhaseZeroAfterSample(1234) == doctest::Approx(1234));

	JSONNode n = describe(s.get());
	CHECK(n.at("mode").as_int() == 1);
	CHECK(n.at("source").as_string() == "constant");
	CHECK(n.at("value").as_float() == doctest::Approx(2.5));
	CHECK(n.at("startSample").as_int() == 0);
	CHECK(n.at("effective").as_bool() == false);
	CHECK(n.at("hint").as_string() == "");
}

TEST_CASE("sine wave values"){
	// offset 1, amplitude 2, period 100, phase 0
	source_ptr s(makeSource(1, "sine", 1, 2, 100, 0, false));
	CHECK(s->displayName() == "sine");
	CHECK(s->getValue(0, 0.001) == doctest::Approx(1.0));          // sin(0)
	CHECK(s->getValue(25, 0.001) == doctest::Approx(3.0));         // peak
	CHECK(s->getValue(50, 0.001) == doctest::Approx(1.0).epsilon(1e-4));
	CHECK(s->getValue(75, 0.001) == doctest::Approx(-1.0));        // trough
	CHECK(s->getValue(100, 0.001) == doctest::Approx(1.0).epsilon(1e-4)); // periodic
}

TEST_CASE("sine wave respects phase"){
	// phase 25 shifts the peak to sample 0
	source_ptr s(makeSource(1, "sine", 0, 1, 100, 25, false));
	CHECK(s->getValue(0, 0.001) == doctest::Approx(1.0));
}

TEST_CASE("sine phase zero after sample"){
	source_ptr s(makeSource(1, "sine", 0, 1, 100, 0, false));
	CHECK(s->getPhaseZeroAfterSample(250) == doctest::Approx(300));
	CHECK(s->getPhaseZeroAfterSample(300) == doctest::Approx(300));
	CHECK(s->getPhaseZeroAfterSample(0) == doctest::Approx(0));
}

TEST_CASE("square wave values"){
	// offset 0.5, amplitude 1, period 10: high for first half, low for second
	source_ptr s(makeSource(1, "square", 0.5, 1, 10, 0, false));
	CHECK(s->displayName() == "square");
	CHECK(s->getValue(0, 0.001) == doctest::Approx(1.5));
	CHECK(s->getValue(4, 0.001) == doctest::Approx(1.5));
	CHECK(s->getValue(5, 0.001) == doctest::Approx(-0.5));
	CHECK(s->getValue(9, 0.001) == doctest::Approx(-0.5));
	CHECK(s->getValue(10, 0.001) == doctest::Approx(1.5));
}

TEST_CASE("square wave phase zero jumps to next period start"){
	source_ptr s(makeSource(1, "square", 0, 1, 10, 0, false));
	CHECK(s->getPhaseZeroAfterSample(3) == doctest::Approx(10));
	// at an exact period start the *next* period is reported
	CHECK(s->getPhaseZeroAfterSample(10) == doctest::Approx(20));
}

TEST_CASE("triangle wave values in steady state"){
	// offset 0, amplitude 1, period 100.
	// Peak at period/4, trough at 3*period/4.
	source_ptr s(makeSource(1, "triangle", 0, 1, 100, 0, false));
	CHECK(s->displayName() == "triangle");
	CHECK(s->getValue(25, 0.001) == doctest::Approx(1.0));
	CHECK(s->getValue(50, 0.001) == doctest::Approx(0.0));
	CHECK(s->getValue(75, 0.001) == doctest::Approx(-1.0));
	CHECK(s->getValue(100, 0.001) == doctest::Approx(0.0));
	CHECK(s->getValue(125, 0.001) == doctest::Approx(1.0));
	// linearity between the extremes
	CHECK(s->getValue(110, 0.001) == doctest::Approx(0.4));
}

TEST_CASE("triangle wave first quarter period"
          * doctest::may_fail()
          * doctest::description("Known bug: fmod of a negative argument makes "
                                 "samples 0..period/4 return offset+amplitude*[1..2] "
                                 "instead of ramping 0..amplitude")){
	source_ptr s(makeSource(1, "triangle", 0, 1, 100, 0, false));
	// Mathematically a triangle starting at 0 should ramp linearly to the
	// peak at period/4.
	CHECK(s->getValue(0, 0.001) == doctest::Approx(0.0));
	CHECK(s->getValue(10, 0.001) == doctest::Approx(0.4));
}

TEST_CASE("periodic relPhase preserves phase fraction across source change"){
	source_ptr prev(makeSource(1, "sine", 0, 1, 10, 0, false));
	// switch at sample 7: old source is at phase fraction 0.7
	source_ptr next(makeSource(1, "sine", 0, 2, 20, 0, true));
	next->initialize(7, prev.get());
	// new source at sample 7 should be at fraction 0.7 of its own period
	double expected = sin(0.7 * 2 * M_PI) * 2;
	CHECK(next->getValue(7, 0.001) == doctest::Approx(expected).epsilon(1e-4));
}

TEST_CASE("periodic without relPhase keeps requested phase"){
	source_ptr prev(makeSource(1, "sine", 0, 1, 10, 0, false));
	source_ptr next(makeSource(1, "sine", 0, 1, 100, 25, false));
	next->initialize(7, prev.get());
	CHECK(next->getValue(0, 0.001) == doctest::Approx(1.0)); // peak from phase 25
}

TEST_CASE("adv_square: low segment comes first"){
	// high=3, low=-1, highSamples=4, lowSamples=6, phase 0
	source_ptr s(makeAdvSquare(1, 3, -1, 4, 6, 0, false));
	CHECK(s->displayName() == "adv_square");
	for (unsigned i = 0; i < 6; i++) CHECK(s->getValue(i, 0.001) == doctest::Approx(-1));
	for (unsigned i = 6; i < 10; i++) CHECK(s->getValue(i, 0.001) == doctest::Approx(3));
	CHECK(s->getValue(10, 0.001) == doctest::Approx(-1)); // wraps
}

TEST_CASE("adv_square zero period throws"){
	CHECK_THROWS_AS(source_ptr(makeAdvSquare(1, 1, 0, 0, 0, 0, false)),
	                ErrorStringException);
}

TEST_CASE("adv_square phase zero is the low-to-high transition"){
	source_ptr s(makeAdvSquare(1, 1, 0, 5, 5, 0, false));
	CHECK(s->getPhaseZeroAfterSample(0) == doctest::Approx(5));
	CHECK(s->getPhaseZeroAfterSample(5) == doctest::Approx(5));
	CHECK(s->getPhaseZeroAfterSample(6) == doctest::Approx(15));
}

TEST_CASE("adv_square describeJSON"){
	source_ptr s(makeAdvSquare(2, 3.5, -1.5, 4, 6, 0, false));
	JSONNode n = describe(s.get());
	CHECK(n.at("source").as_string() == "adv_square");
	CHECK(n.at("high").as_float() == doctest::Approx(3.5));
	CHECK(n.at("low").as_float() == doctest::Approx(-1.5));
	CHECK(n.at("highSamples").as_int() == 4);
	CHECK(n.at("lowSamples").as_int() == 6);
}

TEST_CASE("arb waveform interpolates between points"){
	ArbWavePoint_vec pts;
	pts.push_back(ArbWavePoint(0, 0));
	pts.push_back(ArbWavePoint(10, 10));
	pts.push_back(ArbWavePoint(20, 0));
	source_ptr s(makeArbitraryWaveform(1, 0, pts, 1));
	s->initialize(0, NULL);
	CHECK(s->displayName() == "arb");
	CHECK(s->getValue(0, 0.001) == doctest::Approx(0));
	CHECK(s->getValue(5, 0.001) == doctest::Approx(5));
	CHECK(s->getValue(10, 0.001) == doctest::Approx(10));
	CHECK(s->getValue(15, 0.001) == doctest::Approx(5));
	CHECK(s->getValue(20, 0.001) == doctest::Approx(0));
}

TEST_CASE("arb waveform holds last value when repeat is disabled"){
	ArbWavePoint_vec pts;
	pts.push_back(ArbWavePoint(0, 1));
	pts.push_back(ArbWavePoint(10, 3));
	source_ptr s(makeArbitraryWaveform(1, 0, pts, 1));
	s->initialize(0, NULL);
	CHECK(s->getValue(10, 0.001) == doctest::Approx(3));
	CHECK(s->getValue(100, 0.001) == doctest::Approx(3));
	CHECK(s->getValue(100000, 0.001) == doctest::Approx(3));
}

TEST_CASE("arb waveform repeat count 0 behaves as single pass"){
	ArbWavePoint_vec pts;
	pts.push_back(ArbWavePoint(0, 1));
	pts.push_back(ArbWavePoint(10, 3));
	// repeat 0 is normalized to 1 in the constructor
	source_ptr s(makeArbitraryWaveform(1, 0, pts, 0));
	s->initialize(0, NULL);
	CHECK(s->getValue(15, 0.001) == doctest::Approx(3));
}

TEST_CASE("arb waveform infinite repeat wraps around"){
	ArbWavePoint_vec pts;
	pts.push_back(ArbWavePoint(0, 0));
	pts.push_back(ArbWavePoint(10, 10));
	source_ptr s(makeArbitraryWaveform(1, 0, pts, -1));
	s->initialize(0, NULL);
	CHECK(s->getValue(5, 0.001) == doctest::Approx(5));
	// second period
	CHECK(s->getValue(15, 0.001) == doctest::Approx(5));
	// note: getValue is stateful (walks forward); sample monotonically
	CHECK(s->getValue(25, 0.001) == doctest::Approx(5));
}

TEST_CASE("arb waveform finite repeat then hold"){
	ArbWavePoint_vec pts;
	pts.push_back(ArbWavePoint(0, 0));
	pts.push_back(ArbWavePoint(10, 10));
	source_ptr s(makeArbitraryWaveform(1, 0, pts, 2));
	s->initialize(0, NULL);
	CHECK(s->getValue(5, 0.001) == doctest::Approx(5));   // pass 1
	CHECK(s->getValue(15, 0.001) == doctest::Approx(5));  // pass 2
	CHECK(s->getValue(50, 0.001) == doctest::Approx(10)); // holds last value
}

TEST_CASE("arb waveform validation errors"){
	ArbWavePoint_vec empty;
	CHECK_THROWS_AS(source_ptr(makeArbitraryWaveform(1, 0, empty, 1)),
	                ErrorStringException);

	ArbWavePoint_vec badStart;
	badStart.push_back(ArbWavePoint(5, 0));
	CHECK_THROWS_AS(source_ptr(makeArbitraryWaveform(1, 0, badStart, 1)),
	                ErrorStringException);

	ArbWavePoint_vec unordered;
	unordered.push_back(ArbWavePoint(0, 0));
	unordered.push_back(ArbWavePoint(10, 1));
	unordered.push_back(ArbWavePoint(5, 2));
	CHECK_THROWS_AS(source_ptr(makeArbitraryWaveform(1, 0, unordered, 1)),
	                ErrorStringException);

	// zero period with repeat requested
	ArbWavePoint_vec single;
	single.push_back(ArbWavePoint(0, 1));
	CHECK_THROWS_AS(source_ptr(makeArbitraryWaveform(1, 0, single, -1)),
	                ErrorStringException);
	// but zero period without repeat is allowed
	ArbWavePoint_vec single2;
	single2.push_back(ArbWavePoint(0, 1));
	source_ptr ok(makeArbitraryWaveform(1, 0, single2, 1));
	ok->initialize(0, NULL);
	CHECK(ok->getValue(5, 0.001) == doctest::Approx(1));
}

TEST_CASE("arb describeJSON includes points and period"){
	ArbWavePoint_vec pts;
	pts.push_back(ArbWavePoint(0, 0));
	pts.push_back(ArbWavePoint(10, 10));
	source_ptr s(makeArbitraryWaveform(1, 0, pts, -1));
	s->initialize(0, NULL);
	JSONNode n = describe(s.get());
	CHECK(n.at("source").as_string() == "arb");
	CHECK(n.at("period").as_int() == 10);
	CHECK(n.at("repeat").as_int() == -1);
	JSONNode vals = n.at("values");
	REQUIRE(vals.size() == 2);
	CHECK(vals[0].at("t").as_int() == 0);
	CHECK(vals[1].at("v").as_float() == doctest::Approx(10));
}

// --- makeSource from a JSON description (the WS "set" command payload) ---

TEST_CASE("makeSource JSON: constant with defaults"){
	JSONNode n = jparse("{\"source\": \"constant\", \"mode\": 1}");
	source_ptr s(makeSource(n));
	CHECK(s->mode == 1);
	CHECK(s->getValue(0, 0.001) == doctest::Approx(0)); // default value 0
}

TEST_CASE("makeSource JSON: missing source defaults to constant"){
	JSONNode n = jparse("{\"mode\": 1, \"value\": 3}");
	source_ptr s(makeSource(n));
	CHECK(s->displayName() == "constant");
	CHECK(s->getValue(0, 0.001) == doctest::Approx(3));
}

TEST_CASE("makeSource JSON: sine requires offset, amplitude, period"){
	JSONNode good = jparse("{\"source\": \"sine\", \"mode\": 1, \"offset\": 1, "
	                       "\"amplitude\": 2, \"period\": 100}");
	source_ptr s(makeSource(good));
	CHECK(s->displayName() == "sine");
	CHECK(s->getValue(25, 0.001) == doctest::Approx(3.0));

	JSONNode bad = jparse("{\"source\": \"sine\", \"mode\": 1}");
	CHECK_THROWS_AS(source_ptr(makeSource(bad)), ErrorStringException);
}

TEST_CASE("makeSource JSON: adv_square"){
	JSONNode n = jparse("{\"source\": \"adv_square\", \"mode\": 1, \"high\": 5, "
	                    "\"low\": 0, \"highSamples\": 2, \"lowSamples\": 3}");
	source_ptr s(makeSource(n));
	CHECK(s->displayName() == "adv_square");
	CHECK(s->getValue(0, 0.001) == doctest::Approx(0));
	CHECK(s->getValue(3, 0.001) == doctest::Approx(5));

	JSONNode bad = jparse("{\"source\": \"adv_square\", \"mode\": 1}");
	CHECK_THROWS_AS(source_ptr(makeSource(bad)), ErrorStringException);
}

TEST_CASE("makeSource JSON: arb with values array"){
	JSONNode n = jparse("{\"source\": \"arb\", \"mode\": 1, \"repeat\": 1, \"phase\": 0, "
	                    "\"values\": [{\"t\": 0, \"v\": 1}, {\"t\": 4, \"v\": 5}]}");
	source_ptr s(makeSource(n));
	s->initialize(0, NULL);
	CHECK(s->getValue(2, 0.001) == doctest::Approx(3));
}

TEST_CASE("makeSource JSON: arb missing values throws"){
	JSONNode n = jparse("{\"source\": \"arb\", \"mode\": 1}");
	CHECK_THROWS(source_ptr(makeSource(n)));
}

TEST_CASE("makeSource JSON: invalid source name throws"){
	JSONNode n = jparse("{\"source\": \"sawtooth\", \"mode\": 1}");
	CHECK_THROWS_AS(source_ptr(makeSource(n)), ErrorStringException);
}

TEST_CASE("makeSource JSON: hint is preserved"){
	JSONNode n = jparse("{\"source\": \"constant\", \"mode\": 1, \"value\": 0, "
	                    "\"hint\": \"drag-in-progress\"}");
	source_ptr s(makeSource(n));
	CHECK(s->hint == "drag-in-progress");
	JSONNode d = describe(s.get());
	CHECK(d.at("hint").as_string() == "drag-in-progress");
}

}
