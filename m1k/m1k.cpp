// Nonolith Connect
// M1K (ADALM1000) device support
// Released under the terms of the GNU GPLv3+
// Authors:
//   Based on CEE device by Kevin Mehall <km@kevinmehall.net>
//   M1K protocol from libsmu device_m1000.cpp

#include <stdlib.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <libusb-1.0/libusb.h>
#include <sys/timeb.h>
#include <boost/bind.hpp>
using namespace std;

#include "m1k.hpp"

#define EP_BULK_IN  0x81
#define EP_BULK_OUT 0x02

#ifdef _WIN32
const double M1K_BUFFER_TIME = 0.050;
#else
const double M1K_BUFFER_TIME = 0.020;
#endif

const double M1K_default_sample_time = 1.0 / 100000.0; // 100 ksps

extern "C" void LIBUSB_CALL m1k_in_transfer_callback(libusb_transfer *t);
extern "C" void LIBUSB_CALL m1k_out_transfer_callback(libusb_transfer *t);

static inline float constrain(float val, float lo, float hi) {
	if (val > hi) val = hi;
	if (val < lo) val = lo;
	return val;
}

// --- Constructor / Destructor ---

M1K_device::M1K_device(libusb_device *dev, libusb_device_descriptor &desc):
	StreamingDevice(M1K_default_sample_time),
	USB_device(dev, desc),
	channel_a("a", "A"),
	channel_b("b", "B"),
	//          id   name         unit  min       max       omode uncert.     gain
	channel_a_v("v", "Voltage A", "V",  M1K_V_MIN, M1K_V_MAX, 1, M1K_V_RESOLUTION, 1),
	channel_a_i("i", "Current A", "mA", M1K_I_MIN, M1K_I_MAX, 2, 1, 1),
	channel_b_v("v", "Voltage B", "V",  M1K_V_MIN, M1K_V_MAX, 1, M1K_V_RESOLUTION, 1),
	channel_b_i("i", "Current B", "mA", M1K_I_MIN, M1K_I_MAX, 2, 1, 1)
{
	cerr << "Found an M1K: \n    Serial: " << serial << endl;

	m_mode[0] = M1K_HI_Z;
	m_mode[1] = M1K_HI_Z;
	sof_start = 0;

	// Read hardware and firmware version strings
	char buf[64];
	int r;

	r = controlTransfer(0xC0, 0x00, 0, 0, (uint8_t*)buf, 64);
	if (r >= 0) {
		_hwversion = string(buf, strnlen(buf, r));
	}

	r = controlTransfer(0xC0, 0x00, 0, 1, (uint8_t*)buf, 64);
	if (r >= 0) {
		_fwversion = string(buf, strnlen(buf, r));
	}

	minSampleTime = M1K_MIN_PER / M1K_TIMER_CLOCK;

	cerr << "    Hardware: " << _hwversion << endl;
	cerr << "    Firmware: " << _fwversion << endl;
	cerr << "    Max sample rate: " << M1K_TIMER_CLOCK / M1K_MIN_PER / 1000.0 << " ksps" << endl;

	// Stop any ongoing sampling
	controlTransfer(0x40, 0xC5, 0, 0, 0, 0);

	readCalibration();

	configure(0, M1K_default_sample_time, ceil(12.0 / M1K_default_sample_time), true, false);
}

M1K_device::~M1K_device() {
	pause_capture();
	delete channel_a.source;
	delete channel_b.source;
}

// --- Calibration ---

void M1K_device::readCalibration() {
	uint8_t buf[sizeof(M1K_EEPROM_cal)];
	int r = controlTransfer(0xC0, 0x01, 0, 0, buf, sizeof(buf));
	if (r >= (int)sizeof(M1K_EEPROM_cal)) {
		memcpy(&cal, buf, sizeof(cal));
	}

	if (cal.eeprom_valid != M1K_EEPROM_VALID) {
		cerr << "    Calibration data invalid, using defaults" << endl;
		cal.eeprom_valid = M1K_EEPROM_VALID;
		for (int i = 0; i < 8; i++) {
			cal.offset[i] = 0.0;
			cal.gain_p[i] = 1.0;
			cal.gain_n[i] = 1.0;
		}
	} else {
		cerr << "    Calibration loaded" << endl;
	}
}

// --- Configuration ---

void M1K_device::configure(int mode, double _sampleTime, unsigned samples, bool continuous, bool raw) {
	pause_capture();

	delete channel_a.source;
	channel_a.source = nullptr;
	delete channel_b.source;
	channel_b.source = nullptr;
	channels.clear();
	channel_a.streams.clear();
	channel_b.streams.clear();

	// Compute timer period (matches libsmu: sam_per = round(sample_time * clock) / 2)
	m1k_per = round(_sampleTime * M1K_TIMER_CLOCK) / 2;
	if (m1k_per < M1K_MIN_PER) m1k_per = M1K_MIN_PER;
	if (m1k_per > M1K_MAX_PER) m1k_per = M1K_MAX_PER;
	sampleTime = m1k_per / M1K_TIMER_CLOCK;

	captureSamples = samples;
	captureContinuous = continuous;
	devMode = mode;
	rawMode = raw;
	captureLength = captureSamples * sampleTime;

	packets_per_transfer = ceil(M1K_BUFFER_TIME / (sampleTime * M1K_CHUNK_SIZE) / M1K_N_TRANSFERS);
	if (packets_per_transfer < 1) packets_per_transfer = 1;

	capture_i = capture_o = 0;

	cerr << "M1K configure: per=" << m1k_per
	     << " transfers=" << M1K_N_TRANSFERS
	     << " pkt/xfer=" << packets_per_transfer
	     << " samples=" << captureSamples << endl;

	if (devMode == 0) {
		channels.push_back(&channel_a);
		channel_a.source = makeConstantSource(0, 0);
		channel_a.streams.push_back(&channel_a_v);
		channel_a.streams.push_back(&channel_a_i);

		channels.push_back(&channel_b);
		channel_b.source = makeConstantSource(0, 0);
		channel_b.streams.push_back(&channel_b_v);
		channel_b.streams.push_back(&channel_b_i);

		if (rawMode) {
			channel_a_v.units = channel_b_v.units = string("LSB");
			channel_a_i.units = channel_b_i.units = string("LSB");
			channel_a_v.min = channel_b_v.min = 0;
			channel_a_v.max = channel_b_v.max = 65535;
			channel_a_i.min = channel_b_i.min = 0;
			channel_a_i.max = channel_b_i.max = 65535;
		} else {
			channel_a_v.units = channel_b_v.units = string("V");
			channel_a_i.units = channel_b_i.units = string("mA");
			channel_a_v.min = channel_b_v.min = M1K_V_MIN;
			channel_a_v.max = channel_b_v.max = M1K_V_MAX;
			channel_a_i.min = channel_b_i.min = M1K_I_MIN;
			channel_a_i.max = channel_b_i.max = M1K_I_MAX;
		}

		channel_a_v.allocate(captureSamples);
		channel_a_i.allocate(captureSamples);
		channel_b_v.allocate(captureSamples);
		channel_b_i.allocate(captureSamples);
	}

	notifyConfig();
}

// --- Mode control ---

void M1K_device::set_mode(unsigned channel, unsigned mode) {
	unsigned pset;
	bool split = false;

	switch (mode) {
		case M1K_SIMV_SPLIT: split = true; // fall through
		case M1K_SIMV: pset = 0x7f7f; break;
		case M1K_SVMI_SPLIT: split = true; // fall through
		case M1K_SVMI: pset = 0x0000; break;
		case M1K_HI_Z_SPLIT: split = true; // fall through
		case M1K_HI_Z:
		default: pset = 0x3000; break;
	}

	// Set feedback potentiometers
	controlTransfer(0x40, 0x59, channel, pset, 0, 0);

	// Set mode (firmware only understands 0/1/2)
	controlTransfer(0x40, 0x53, channel, mode % 3, 0, 0);

	// Set SPLIT pin for split modes
	if (split) {
		unsigned pin = (channel == 0) ? 34 : 39;
		controlTransfer(0x40, 0x51, pin, 0, 0, 0);
	}

	m_mode[channel] = mode;
}

// --- Capture control ---

void M1K_device::on_reset_capture() {
	boost::mutex::scoped_lock lock(outputMutex);
	incount = outcount = 0;
	if (channel_a.source) channel_a.source->startSample = 0;
	if (channel_b.source) channel_b.source->startSample = 0;
}

void M1K_device::on_start_capture() {
	claimInterface();

	// Set alt setting 1 for streaming
	int r = libusb_set_interface_alt_setting(handle, 0, 1);
	if (r != 0) {
		cerr << "M1K: could not set alt setting 1: " << r << endl;
	}

	boost::mutex::scoped_lock lock(transfersMutex);

	// Set channel modes
	set_mode(0, m_mode[0]);
	set_mode(1, m_mode[1]);

	// Stop any ongoing sampling
	controlTransfer(0x40, 0xC5, 0, 0, 0, 0);

	// Initialize hardware
	controlTransfer(0x40, 0xCC, 0, 0, 0, 0);

	// Sync SOF
	controlTransfer(0xC0, 0x6F, 0, 0, (uint8_t*)&sof_start, 2);
	sof_start = (((sof_start >> 3) + 0x1f) & 0x7FF) << 3;

	// Start sampling
	controlTransfer(0x40, 0xC5, m1k_per, sof_start, 0, 0);

	// Ignore output samples sent before pausing
	capture_o = capture_i;

	for (int i = 0; i < M1K_N_TRANSFERS; i++) {
		// IN transfers
		in_transfers[i] = libusb_alloc_transfer(0);
		const int isize = M1K_IN_PACKET_SIZE * packets_per_transfer;
		unsigned char* ibuf = (unsigned char*)malloc(isize);
		libusb_fill_bulk_transfer(in_transfers[i], handle, EP_BULK_IN,
			ibuf, isize, m1k_in_transfer_callback, this, 500);
		in_transfers[i]->flags |= LIBUSB_TRANSFER_FREE_BUFFER;
		libusb_submit_transfer(in_transfers[i]);

		// OUT transfers
		out_transfers[i] = libusb_alloc_transfer(0);
		const int osize = M1K_OUT_PACKET_SIZE * packets_per_transfer;
		unsigned char* obuf = (unsigned char*)malloc(osize);
		fillOutTransfer(obuf);
		outcount++;
		libusb_fill_bulk_transfer(out_transfers[i], handle, EP_BULK_OUT,
			obuf, osize, m1k_out_transfer_callback, this, 500);
		out_transfers[i]->flags |= LIBUSB_TRANSFER_FREE_BUFFER;
		libusb_submit_transfer(out_transfers[i]);
	}
}

void M1K_device::on_pause_capture() {
	boost::mutex::scoped_lock lock(transfersMutex);

	// Stop sampling
	controlTransfer(0x40, 0xC5, 0, 0, 0, 0);

	// Reset modes to HI_Z
	set_mode(0, M1K_HI_Z);
	set_mode(1, M1K_HI_Z);

	for (int i = 0; i < M1K_N_TRANSFERS; i++) {
		if (in_transfers[i]) {
			in_transfers[i]->user_data = 0;
			libusb_cancel_transfer(in_transfers[i]);
			in_transfers[i] = 0;
		}
		if (out_transfers[i]) {
			out_transfers[i]->user_data = 0;
			libusb_cancel_transfer(out_transfers[i]);
			out_transfers[i] = 0;
		}
	}

	capture_o = capture_i;
	releaseInterface();
}

// --- IN transfer decoding ---

void M1K_device::handleInTransfer(unsigned char* buffer) {
	for (int p = 0; p < packets_per_transfer; p++) {
		uint8_t* buf = buffer + p * M1K_IN_PACKET_SIZE;

		for (unsigned i = 0; i < M1K_CHUNK_SIZE; i++) {
			float v;

			if (rawMode) {
				put(channel_a_v, (buf[i*8+0] << 8 | buf[i*8+1]));
				put(channel_a_i, (buf[i*8+2] << 8 | buf[i*8+3]));
				put(channel_b_v, (buf[i*8+4] << 8 | buf[i*8+5]));
				put(channel_b_i, (buf[i*8+6] << 8 | buf[i*8+7]));
			} else {
				// Channel A voltage
				v = (buf[i*8+0] << 8 | buf[i*8+1]) * M1K_V_RESOLUTION;
				put(channel_a_v, (v - cal.offset[0]) * cal.gain_p[0]);

				// Channel A current: scale to mA
				v = (((buf[i*8+2] << 8 | buf[i*8+3]) * M1K_I_RESOLUTION) - 0.195) * 1.25;
				float ai = (v - cal.offset[1]) * (v > 0 ? cal.gain_p[1] : cal.gain_n[1]);
				put(channel_a_i, ai * 1000.0); // A -> mA

				// Channel B voltage
				v = (buf[i*8+4] << 8 | buf[i*8+5]) * M1K_V_RESOLUTION;
				put(channel_b_v, (v - cal.offset[4]) * cal.gain_p[4]);

				// Channel B current: scale to mA
				v = (((buf[i*8+6] << 8 | buf[i*8+7]) * M1K_I_RESOLUTION) - 0.195) * 1.25;
				float bi = (v - cal.offset[5]) * (v > 0 ? cal.gain_p[5] : cal.gain_n[5]);
				put(channel_b_i, bi * 1000.0); // A -> mA
			}

			sampleDone();
		}
	}

	free(buffer);
	packetDone();
	checkOutputEffective(channel_a);
	checkOutputEffective(channel_b);
}

// --- OUT transfer encoding ---

uint16_t M1K_device::encode_out(unsigned channel, float val) {
	int v = 32768 * 4 / 5; // HI_Z midscale default

	if (rawMode) {
		return constrain(val, 0, 65535);
	}

	unsigned mode = m_mode[channel];

	if (mode == M1K_SVMI || mode == M1K_SVMI_SPLIT) {
		// val is in V, apply source calibration
		val = (val - cal.offset[channel*4+2]) * cal.gain_p[channel*4+2];
		val = constrain(val, M1K_V_MIN, M1K_V_MAX);
		v = val * (1.0 / M1K_V_RESOLUTION);
	} else if (mode == M1K_SIMV || mode == M1K_SIMV_SPLIT) {
		// val is in mA, convert to A for encoding
		float val_a = val / 1000.0;
		if (val_a > 0) {
			val_a = (val_a - cal.offset[channel*4+3]) * cal.gain_p[channel*4+3];
		} else {
			val_a = (val_a - cal.offset[channel*4+3]) * cal.gain_n[channel*4+3];
		}
		val_a = constrain(val_a, -0.2, 0.2);
		v = 65536 * (2.0/5.0 + 0.8*0.2*20.0*0.5*val_a);
	}

	v = constrain(v, 0, 65535);
	return v;
}

void M1K_device::fillOutTransfer(unsigned char* buf) {
	boost::mutex::scoped_lock lock(outputMutex);

	if (channel_a.source && channel_b.source) {
		for (int p = 0; p < packets_per_transfer; p++) {
			uint8_t* pkt = buf + p * M1K_OUT_PACKET_SIZE;

			for (unsigned i = 0; i < M1K_CHUNK_SIZE; i++) {
				uint16_t a = encode_out(0, channel_a.source->getValue(capture_o, sampleTime));
				uint16_t b = encode_out(1, channel_b.source->getValue(capture_o, sampleTime));

				// 16-bit big-endian interleaved: [A_hi, A_lo, B_hi, B_lo]
				pkt[i*4+0] = a >> 8;
				pkt[i*4+1] = a & 0xff;
				pkt[i*4+2] = b >> 8;
				pkt[i*4+3] = b & 0xff;

				capture_o++;
			}
		}
	} else {
		memset(buf, 0, M1K_OUT_PACKET_SIZE * packets_per_transfer);
	}
}

// --- Output management ---

void M1K_device::setOutput(Channel* channel, OutputSource* source) {
	{
		boost::mutex::scoped_lock lock(outputMutex);
		source->initialize(capture_o, channel->source);
		if (channel->source) {
			delete channel->source;
		}
		channel->source = source;
		channel->source->startSample = capture_o + 1;
	}

	// Update hardware mode if the output mode changed
	unsigned modeVal = source->mode;
	if (channel == &channel_a && m_mode[0] != modeVal) {
		if (captureState) {
			set_mode(0, modeVal);
		} else {
			m_mode[0] = modeVal;
		}
	} else if (channel == &channel_b && m_mode[1] != modeVal) {
		if (captureState) {
			set_mode(1, modeVal);
		} else {
			m_mode[1] = modeVal;
		}
	}

	notifyOutputChanged(channel, source);
}

inline void M1K_device::checkOutputEffective(Channel& channel) {
	if (!channel.source->effective && capture_i > channel.source->startSample) {
		channel.source->effective = true;
		notifyOutputChanged(&channel, channel.source);
	}
}

// --- WebSocket message handling ---

bool M1K_device::processMessage(ClientConn& client, string& cmd, JSONNode& n) {
	if (cmd == "readCalibration") {
		JSONNode reply(JSON_NODE);
		reply.push_back(JSONNode("_action", "return"));
		reply.push_back(JSONNode("id", jsonIntProp(n, "id", 0)));

		JSONNode offsets(JSON_ARRAY);
		offsets.set_name("offset");
		JSONNode gp(JSON_ARRAY);
		gp.set_name("gain_p");
		JSONNode gn(JSON_ARRAY);
		gn.set_name("gain_n");

		for (int i = 0; i < 8; i++) {
			offsets.push_back(JSONNode("", cal.offset[i]));
			gp.push_back(JSONNode("", cal.gain_p[i]));
			gn.push_back(JSONNode("", cal.gain_n[i]));
		}

		reply.push_back(offsets);
		reply.push_back(gp);
		reply.push_back(gn);
		reply.push_back(JSONNode("valid", cal.eeprom_valid == M1K_EEPROM_VALID));

		client.sendJSON(reply);
		return true;

	} else if (cmd == "writeCalibration") {
		JSONNode j_off = n.at("offset");
		JSONNode j_gp = n.at("gain_p");
		JSONNode j_gn = n.at("gain_n");

		for (int i = 0; i < 8; i++) {
			cal.offset[i] = j_off[i].as_float();
			cal.gain_p[i] = j_gp[i].as_float();
			cal.gain_n[i] = j_gn[i].as_float();
		}
		cal.eeprom_valid = M1K_EEPROM_VALID;

		int r = controlTransfer(0x40, 0x02, 0, 0, (uint8_t*)&cal, sizeof(cal));

		JSONNode reply(JSON_NODE);
		reply.push_back(JSONNode("_action", "return"));
		reply.push_back(JSONNode("id", jsonIntProp(n, "id", 0)));
		reply.push_back(JSONNode("status", r));
		client.sendJSON(reply);
		return true;

	} else {
		return USB_device::processMessage(client, cmd, n)
			|| StreamingDevice::processMessage(client, cmd, n);
	}
}

// --- REST API ---

bool M1K_device::handleREST(UrlPath path, session_ptr client) {
	return StreamingDevice::handleREST(path, client);
}

// --- USB transfer callbacks ---

static void destroy_m1k_transfer(M1K_device* dev, libusb_transfer** list, libusb_transfer* t) {
	boost::mutex::scoped_lock lock(dev->transfersMutex);
	t->user_data = 0;
	for (int i = 0; i < M1K_N_TRANSFERS; i++) {
		if (list[i] == t) {
			list[i] = 0;
		}
	}
	libusb_free_transfer(t);
}

extern "C" void LIBUSB_CALL m1k_in_transfer_callback(libusb_transfer *t) {
	if (!t->user_data) {
		libusb_free_transfer(t);
		return;
	}

	M1K_device *dev = (M1K_device*)t->user_data;

	if (t->status == LIBUSB_TRANSFER_COMPLETED) {
		boost::asio::post(io, boost::bind(&M1K_device::handleInTransfer, dev, t->buffer));
		t->buffer = (unsigned char*)malloc(M1K_IN_PACKET_SIZE * dev->packets_per_transfer);

		if (dev->captureContinuous || dev->incount * M1K_CHUNK_SIZE < dev->captureSamples) {
			dev->incount++;
			libusb_submit_transfer(t);
		} else {
			destroy_m1k_transfer(dev, dev->in_transfers, t);
		}
	} else {
		cerr << "M1K IN transfer error: " << t->status << endl;
		destroy_m1k_transfer(dev, dev->in_transfers, t);
	}
}

extern "C" void LIBUSB_CALL m1k_out_transfer_callback(libusb_transfer *t) {
	if (!t->user_data) {
		libusb_free_transfer(t);
		return;
	}

	M1K_device *dev = (M1K_device*)t->user_data;

	if (t->status == LIBUSB_TRANSFER_COMPLETED) {
		if (dev->captureContinuous || dev->outcount * M1K_CHUNK_SIZE < dev->captureSamples) {
			dev->fillOutTransfer(t->buffer);
			dev->outcount++;
			libusb_submit_transfer(t);
		}
	} else {
		cerr << "M1K OUT transfer error: " << t->status << endl;
		destroy_m1k_transfer(dev, dev->out_transfers, t);
	}
}
