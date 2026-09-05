// Tests for device lookup/serialization (device.cpp) and the Event
// pub/sub helper (event.cpp).

#include "support.hpp"

TEST_SUITE("device"){

TEST_CASE("getDeviceById matches exact ids"){
	test_device_ptr dev = makeTestDevice();
	DeviceRegistration reg(dev);
	CHECK(getDeviceById("com.nonolithlabs.test~TEST001") == dev);
	CHECK(getDeviceById("com.nonolithlabs.test~OTHER") == device_ptr());
	CHECK(getDeviceById("nope") == device_ptr());
}

TEST_CASE("getDeviceById supports model wildcards"){
	test_device_ptr dev = makeTestDevice();
	DeviceRegistration reg(dev);
	CHECK(getDeviceById("com.nonolithlabs.test*") == dev);
	CHECK(getDeviceById("com.nonolithlabs.other*") == device_ptr());
}

TEST_CASE("Device::toJSON carries identification fields"){
	test_device_ptr dev = makeTestDevice();
	JSONNode n = dev->toJSON();
	CHECK(n.at("id").as_string() == "com.nonolithlabs.test~TEST001");
	CHECK(n.at("model").as_string() == "com.nonolithlabs.test");
	CHECK(n.at("hwVersion").as_string() == "T1");
	CHECK(n.at("fwVersion").as_string() == "9.9");
	CHECK(n.at("serial").as_string() == "TEST001");
}

TEST_CASE("jsonDevicesArray keys devices by id"){
	test_device_ptr dev = makeTestDevice();
	DeviceRegistration reg(dev);
	JSONNode n = jsonDevicesArray();
	CHECK(n.size() == 1);
	CHECK(n.at("com.nonolithlabs.test~TEST001").at("serial").as_string() == "TEST001");
}

TEST_CASE("broadcastJSON reaches all attached clients"){
	test_device_ptr dev = makeTestDevice();
	FakeClientConn c1, c2;
	c1.selectDevice(dev);
	c2.selectDevice(dev);
	c1.clear();
	c2.clear();

	dev->start_capture();
	CHECK(c1.actions("captureState").size() == 1);
	CHECK(c2.actions("captureState").size() == 1);
}

}

TEST_SUITE("event"){

TEST_CASE("notify calls all subscribed handlers"){
	Event e;
	int calls1 = 0, calls2 = 0;
	EventListener l1(e, [&]{ calls1++; });
	EventListener l2(e, [&]{ calls2++; });
	e.notify();
	CHECK(calls1 == 1);
	CHECK(calls2 == 1);
	e.notify();
	CHECK(calls1 == 2);
}

TEST_CASE("unsubscribe stops notifications"){
	Event e;
	int calls = 0;
	EventListener l(e, [&]{ calls++; });
	l.unsubscribe();
	e.notify();
	CHECK(calls == 0);
}

TEST_CASE("listener destruction unsubscribes"){
	Event e;
	int calls = 0;
	{
		EventListener l(e, [&]{ calls++; });
	}
	e.notify();
	CHECK(calls == 0);
	CHECK(e.listeners.empty());
}

TEST_CASE("a handler may unsubscribe itself during notify"){
	Event e;
	int calls = 0;
	EventListener l;
	l.subscribe(e, [&]{ calls++; l.unsubscribe(); });
	e.notify();
	e.notify();
	CHECK(calls == 1);
}

TEST_CASE("resubscribing moves the listener to the new event"){
	Event e1, e2;
	int calls = 0;
	EventListener l(e1, [&]{ calls++; });
	l.subscribe(e2, [&]{ calls += 10; });
	e1.notify();
	CHECK(calls == 0);
	e2.notify();
	CHECK(calls == 10);
}

TEST_CASE("event destruction detaches listeners safely"){
	EventListener l;
	{
		Event e;
		l.subscribe(e, []{});
	}
	CHECK(l.event == (Event*) 0);
	// destroying l afterwards must not touch the dead event
}

}
