// Tests for typed JSON property extraction (json_helpers.hpp)

#include "support.hpp"

TEST_SUITE("json_helpers"){

static JSONNode sample(){
	return jparse("{\"s\": \"hello\", \"i\": 42, \"f\": 2.5, \"b\": true, \"nil\": null}");
}

TEST_CASE("jsonStringProp"){
	JSONNode n = sample();
	CHECK(jsonStringProp(n, "s") == "hello");
	CHECK(jsonStringProp(n, "s", "def") == "hello");
	CHECK(jsonStringProp(n, "missing", "def") == "def");
	CHECK_THROWS_AS(jsonStringProp(n, "missing"), ErrorStringException);
	// wrong type: number is not a string
	CHECK_THROWS_AS(jsonStringProp(n, "i"), ErrorStringException);
	CHECK(jsonStringProp(n, "i", "def") == "def");
}

TEST_CASE("jsonIntProp"){
	JSONNode n = sample();
	CHECK(jsonIntProp(n, "i") == 42);
	CHECK(jsonIntProp(n, "missing", 7) == 7);
	CHECK_THROWS_AS(jsonIntProp(n, "missing"), ErrorStringException);
	CHECK_THROWS_AS(jsonIntProp(n, "s"), ErrorStringException);
	// float value truncates via as_int
	CHECK(jsonIntProp(n, "f") == 2);
}

TEST_CASE("jsonFloatProp"){
	JSONNode n = sample();
	CHECK(jsonFloatProp(n, "f") == doctest::Approx(2.5));
	CHECK(jsonFloatProp(n, "i") == doctest::Approx(42.0));
	CHECK(jsonFloatProp(n, "missing", 1.5) == doctest::Approx(1.5));
	CHECK_THROWS_AS(jsonFloatProp(n, "missing"), ErrorStringException);
	CHECK_THROWS_AS(jsonFloatProp(n, "b"), ErrorStringException);
}

TEST_CASE("jsonBoolProp"){
	JSONNode n = sample();
	CHECK(jsonBoolProp(n, "b") == true);
	CHECK(jsonBoolProp(n, "missing", true) == true);
	CHECK(jsonBoolProp(n, "missing", false) == false);
	CHECK_THROWS_AS(jsonBoolProp(n, "missing"), ErrorStringException);
	// wrong type: int is not a bool
	CHECK_THROWS_AS(jsonBoolProp(n, "i"), ErrorStringException);
}

TEST_CASE("ErrorStringException carries the message"){
	ErrorStringException e("some message");
	CHECK(std::string(e.what()) == "some message");
}

}
