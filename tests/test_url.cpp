// Tests for URL and query-string parsing (url.cpp / url.hpp)

#include "support.hpp"

TEST_SUITE("url"){

TEST_CASE("parse_query basic pairs"){
	std::map<string, string> m;
	parse_query("a=1&b=two&c=3.5", m);
	CHECK(m.size() == 3);
	CHECK(m["a"] == "1");
	CHECK(m["b"] == "two");
	CHECK(m["c"] == "3.5");
}

TEST_CASE("parse_query empty string yields no entries"){
	std::map<string, string> m;
	parse_query("", m);
	CHECK(m.empty());
}

TEST_CASE("parse_query pair without '=' is ignored"){
	std::map<string, string> m;
	parse_query("novalue&x=1", m);
	CHECK(m.size() == 1);
	CHECK(m["x"] == "1");
}

TEST_CASE("parse_query value may contain '='"){
	std::map<string, string> m;
	parse_query("k=a=b", m);
	CHECK(m["k"] == "a=b");
}

TEST_CASE("parse_query empty value"){
	std::map<string, string> m;
	parse_query("k=", m);
	CHECK(m.count("k") == 1);
	CHECK(m["k"] == "");
}

TEST_CASE("parse_query later duplicate key wins"){
	std::map<string, string> m;
	parse_query("k=1&k=2", m);
	CHECK(m["k"] == "2");
}

TEST_CASE("map_get returns default for missing key"){
	std::map<string, string> m;
	m["present"] = "yes";
	CHECK(map_get(m, "present", "def") == "yes");
	CHECK(map_get(m, "absent", "def") == "def");
	CHECK(map_get(m, "absent") == "");
}

TEST_CASE("map_get_num parses numeric types"){
	std::map<string, string> m;
	m["i"] = "42";
	m["f"] = "2.5";
	m["neg"] = "-7";
	CHECK(map_get_num(m, "i", 0) == 42);
	CHECK(map_get_num(m, "f", 0.0) == doctest::Approx(2.5));
	CHECK(map_get_num(m, "neg", 0) == -7);
	CHECK(map_get_num(m, "missing", 13) == 13);
}

TEST_CASE("map_get_num throws on non-numeric value"){
	std::map<string, string> m;
	m["bad"] = "xyz";
	CHECK_THROWS(map_get_num(m, "bad", 0));
}

TEST_CASE("Url splits path and parses query"){
	Url u("/rest/v1/devices?x=1&y=2");
	REQUIRE(u.pathparts.size() == 4);
	CHECK(u.pathparts[0] == "");
	CHECK(u.pathparts[1] == "rest");
	CHECK(u.pathparts[2] == "v1");
	CHECK(u.pathparts[3] == "devices");
	CHECK(u.params["x"] == "1");
	CHECK(u.params["y"] == "2");
}

TEST_CASE("Url removes single trailing slash"){
	Url u("/rest/v1/");
	REQUIRE(u.pathparts.size() == 3);
	CHECK(u.pathparts[2] == "v1");
}

TEST_CASE("Url root path"){
	Url u("/");
	// "/" splits to ["", ""], trailing empty removed
	CHECK(u.pathparts.size() == 1);
	CHECK(u.pathparts[0] == "");
}

TEST_CASE("Url without query has empty params"){
	Url u("/a/b");
	CHECK(u.params.empty());
}

TEST_CASE("UrlPath navigation: leaf, matches, sub, get, param"){
	Url u("/rest/v1/devices/dev1?resample=0.01");
	UrlPath p(u, 1);
	CHECK(!p.leaf());
	CHECK(p.matches("rest"));
	CHECK(p.get() == "rest");

	UrlPath p2 = p.sub();
	CHECK(p2.matches("v1"));

	UrlPath p3 = p2.sub();
	CHECK(p3.matches("devices"));

	UrlPath p4 = p3.sub();
	CHECK(p4.get() == "dev1");
	CHECK(!p4.leaf());

	UrlPath p5 = p4.sub();
	CHECK(p5.leaf());

	CHECK(p.param("resample") == "0.01");
	CHECK(p.param("missing", "def") == "def");
}

TEST_CASE("UrlPath get() past the end throws"){
	Url u("/rest");
	UrlPath p(u, 5);
	CHECK(p.leaf());
	CHECK_THROWS(p.get());
}

}
