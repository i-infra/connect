// Tests for top-level REST routing (rest_service.cpp): /rest/v1/...

#include "support.hpp"

TEST_SUITE("rest_service"){

TEST_CASE("GET /rest/v1/ returns server identification"){
	fake_session_ptr r = restServiceRequest("/rest/v1/");
	CHECK(r->status == 200);
	JSONNode n = r->jsonBody();
	CHECK(n.at("server").as_string() == "Nonolith Connect");
	CHECK(jhas(n, "version"));
	CHECK(jhas(n, "gitVersion"));
}

TEST_CASE("GET /rest with no version is a 404"){
	fake_session_ptr r = restServiceRequest("/rest");
	CHECK(r->status == 404);
	CHECK(r->body == "No API version selected.");
}

TEST_CASE("unsupported API version is a 404"){
	fake_session_ptr r = restServiceRequest("/rest/v2/devices");
	CHECK(r->status == 404);
	CHECK(r->body == "API version not supported");
}

TEST_CASE("GET /rest/v1/devices lists registered devices"){
	test_device_ptr dev = makeTestDevice();
	DeviceRegistration reg(dev);

	fake_session_ptr r = restServiceRequest("/rest/v1/devices");
	CHECK(r->status == 200);
	JSONNode n = r->jsonBody();
	JSONNode d = n.at("com.nonolithlabs.test~TEST001");
	CHECK(d.at("model").as_string() == "com.nonolithlabs.test");
	CHECK(d.at("serial").as_string() == "TEST001");
	CHECK(d.at("hwVersion").as_string() == "T1");
	CHECK(d.at("fwVersion").as_string() == "9.9");
}

TEST_CASE("GET /rest/v1/devices with no devices is an empty object"){
	fake_session_ptr r = restServiceRequest("/rest/v1/devices");
	CHECK(r->status == 200);
	CHECK(r->jsonBody().size() == 0);
}

TEST_CASE("device requests are routed by exact id"){
	test_device_ptr dev = makeTestDevice();
	DeviceRegistration reg(dev);

	fake_session_ptr r = restServiceRequest("/rest/v1/devices/com.nonolithlabs.test~TEST001");
	CHECK(r->status == 200);
	CHECK(r->jsonBody().at("serial").as_string() == "TEST001");

	// deeper paths reach the device's channel resources
	fake_session_ptr r2 = restServiceRequest("/rest/v1/devices/com.nonolithlabs.test~TEST001/a");
	CHECK(r2->status == 200);
	CHECK(r2->jsonBody().at("id").as_string() == "a");
}

TEST_CASE("device requests support model wildcard ids"){
	test_device_ptr dev = makeTestDevice();
	DeviceRegistration reg(dev);

	fake_session_ptr r = restServiceRequest("/rest/v1/devices/com.nonolithlabs.test*");
	CHECK(r->status == 200);
	CHECK(r->jsonBody().at("serial").as_string() == "TEST001");
}

TEST_CASE("unknown device id is a 404"){
	fake_session_ptr r = restServiceRequest("/rest/v1/devices/does.not.exist~X");
	CHECK(r->status == 404);
	CHECK(r->body == "REST object not found");
}

TEST_CASE("unhandled device subresource is a 404"){
	test_device_ptr dev = makeTestDevice();
	DeviceRegistration reg(dev);
	fake_session_ptr r = restServiceRequest("/rest/v1/devices/com.nonolithlabs.test~TEST001/bogus");
	CHECK(r->status == 404);
	CHECK(r->body == "REST object not found");
}

TEST_CASE("Access-Control-Allow-Origin echoes the request origin"){
	fake_session_ptr s = std::make_shared<FakeSession>();
	s->resource = "/rest/v1/";
	s->clientHeaders["Origin"] = "http://localhost:5173";
	Url url(s->resource);
	UrlPath path(url, 1);
	handleJSONRequest(path.sub(), s);
	CHECK(s->responseHeaders["Access-Control-Allow-Origin"] == "http://localhost:5173");
}

}
