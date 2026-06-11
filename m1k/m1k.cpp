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
	channel_a_i("i", "Current A", "mA", M1K_I_MIN, M1K_I_MAX, 2, M1K_I_RESOLUTION*1000.0, 1),
	channel_b_v("v", "Voltage B", "V",  M1K_V_MIN, M1K_V_MAX, 1, M1K_V_RESOLUTION, 1),
	channel_b_i("i", "Current B", "mA", M1K_I_MIN, M1K_I_MAX, 2, M1K_I_RESOLUTION*1000.0, 1)
{
	cerr << "Found an M1K: \n    Serial: " << serial << endl;

	m_mode[0] = M1K_HI_Z;
	m_mode[1] = M1K_HI_Z;
	sof_start = 0;
	ledState = 0;

	// Init frontend state to match firmware defaults
	for (int ch = 0; ch < 2; ch++) {
		frontend[ch].r50_2v5 = false;
		frontend[ch].r50_gnd = false;
		frontend[ch].feedback = true;  // active LOW, initialized LOW = on
		frontend[ch].output_en = false;
		frontend[ch].split = false;
		frontend[ch].pot_r1 = 0;
		frontend[ch].pot_r2 = 0;
	}

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

	// Firmware >= 2.0 uses interleaved data format; older uses block format
	fw_interleaved = (atof(_fwversion.c_str()) >= 2.0);

	minSampleTime = 2.0 * M1K_MIN_PER / M1K_TIMER_CLOCK;

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
		// Leave eeprom_valid as-is so clients querying readCalibration can
		// see that the device is running on defaults; writeCalibration sets
		// the magic before writing.
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
	// Each sample takes 2 timer ticks (A-phase + B-phase), so actual
	// sample period = 2 * m1k_per / clock.
	m1k_per = round(_sampleTime * M1K_TIMER_CLOCK) / 2;
	if (m1k_per < M1K_MIN_PER) m1k_per = M1K_MIN_PER;
	if (m1k_per > M1K_MAX_PER) m1k_per = M1K_MAX_PER;
	sampleTime = 2.0 * m1k_per / M1K_TIMER_CLOCK;

	captureSamples = samples;
	captureContinuous = continuous;
	if (mode != 0) {
		// M1K only implements mode 0; anything else would leave channels
		// and sample buffers unconfigured and crash the capture path
		cerr << "M1K: unsupported mode " << mode << " requested, using 0" << endl;
		mode = 0;
	}
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

// --- State JSON (includes frontend) ---

JSONNode M1K_device::stateToJSON(bool configOnly) {
	JSONNode n = StreamingDevice::stateToJSON(configOnly);

	// Append M1K-specific frontend state
	JSONNode fe = frontendToJSON();
	fe.set_name("frontend");
	n.push_back(fe);

	// Include current mode names for clarity
	JSONNode modes(JSON_NODE);
	modes.set_name("m1k_modes");
	const char* mode_names[] = {"hi_z", "svmi", "simv", "hi_z_split", "svmi_split", "simv_split"};
	modes.push_back(JSONNode("a", mode_names[m_mode[0]]));
	modes.push_back(JSONNode("b", mode_names[m_mode[1]]));
	n.push_back(modes);

	return n;
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

	// Update frontend cache to match what firmware set_mode() does to the GPIO pins
	// Firmware set_mode: DISABLED => feedback on (PBx clear), output_en off (PBx set)
	//                    SVMI/SIMV => feedback on (PBx clear), output_en on (PBx clear)
	unsigned base_mode = mode % 3; // 0=HI_Z, 1=SVMI, 2=SIMV
	frontend[channel].feedback = true; // firmware always clears feedback pin (active LOW = on)
	frontend[channel].output_en = (base_mode != 0); // on for SVMI/SIMV, off for HI_Z/DISABLED
	frontend[channel].split = split;
	frontend[channel].pot_r1 = (pset >> 8) & 0xFF;
	frontend[channel].pot_r2 = pset & 0xFF;
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
			ibuf, isize, m1k_in_transfer_callback, this, 10000);
		in_transfers[i]->flags |= LIBUSB_TRANSFER_FREE_BUFFER;
		libusb_submit_transfer(in_transfers[i]);

		// OUT transfers
		out_transfers[i] = libusb_alloc_transfer(0);
		const int osize = M1K_OUT_PACKET_SIZE * packets_per_transfer;
		unsigned char* obuf = (unsigned char*)malloc(osize);
		fillOutTransfer(obuf);
		outcount++;
		libusb_fill_bulk_transfer(out_transfers[i], handle, EP_BULK_OUT,
			obuf, osize, m1k_out_transfer_callback, this, 10000);
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

			uint16_t raw_av, raw_ai, raw_bv, raw_bi;

			if (fw_interleaved) {
				// Firmware >= 2.0: interleaved [AV, AI, BV, BI] per sample
				raw_av = buf[i*8+0] << 8 | buf[i*8+1];
				raw_ai = buf[i*8+2] << 8 | buf[i*8+3];
				raw_bv = buf[i*8+4] << 8 | buf[i*8+5];
				raw_bi = buf[i*8+6] << 8 | buf[i*8+7];
			} else {
				// Firmware < 2.0: block format [all AV][all AI][all BV][all BI]
				raw_av = buf[(i+M1K_CHUNK_SIZE*0)*2] << 8 | buf[(i+M1K_CHUNK_SIZE*0)*2+1];
				raw_ai = buf[(i+M1K_CHUNK_SIZE*1)*2] << 8 | buf[(i+M1K_CHUNK_SIZE*1)*2+1];
				raw_bv = buf[(i+M1K_CHUNK_SIZE*2)*2] << 8 | buf[(i+M1K_CHUNK_SIZE*2)*2+1];
				raw_bi = buf[(i+M1K_CHUNK_SIZE*3)*2] << 8 | buf[(i+M1K_CHUNK_SIZE*3)*2+1];
			}

			if (rawMode) {
				put(channel_a_v, raw_av);
				put(channel_a_i, raw_ai);
				put(channel_b_v, raw_bv);
				put(channel_b_i, raw_bi);
			} else {
				// Channel A voltage
				v = raw_av * M1K_V_RESOLUTION;
				put(channel_a_v, (v - cal.offset[0]) * cal.gain_p[0]);

				// Channel A current: scale to mA
				v = ((raw_ai * M1K_I_RESOLUTION) - 0.195) * 1.25;
				float ai = (v - cal.offset[1]) * (v > 0 ? cal.gain_p[1] : cal.gain_n[1]);
				put(channel_a_i, ai * 1000.0); // A -> mA

				// Channel B voltage
				v = raw_bv * M1K_V_RESOLUTION;
				put(channel_b_v, (v - cal.offset[4]) * cal.gain_p[4]);

				// Channel B current: scale to mA
				v = ((raw_bi * M1K_I_RESOLUTION) - 0.195) * 1.25;
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

				if (fw_interleaved) {
					// Firmware >= 2.0: interleaved [A_hi, A_lo, B_hi, B_lo]
					pkt[i*4+0] = a >> 8;
					pkt[i*4+1] = a & 0xff;
					pkt[i*4+2] = b >> 8;
					pkt[i*4+3] = b & 0xff;
				} else {
					// Firmware < 2.0: block [all A][all B]
					pkt[(i+M1K_CHUNK_SIZE*0)*2]   = a >> 8;
					pkt[(i+M1K_CHUNK_SIZE*0)*2+1] = a & 0xff;
					pkt[(i+M1K_CHUNK_SIZE*1)*2]   = b >> 8;
					pkt[(i+M1K_CHUNK_SIZE*1)*2+1] = b & 0xff;
				}

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
	// source is null if configured with an unsupported mode (only mode 0
	// sets up channels for the M1K)
	if (!channel.source) return;
	if (!channel.source->effective && capture_i > channel.source->startSample) {
		channel.source->effective = true;
		notifyOutputChanged(&channel, channel.source);
	}
}

// --- GPIO helpers ---

void M1K_device::setGPIO(unsigned pin, bool high) {
	controlTransfer(0x40, high ? 0x51 : 0x50, pin, 0, 0, 0);
}

void M1K_device::setDigipot(unsigned channel, uint8_t r1, uint8_t r2) {
	controlTransfer(0x40, 0x59, channel, (r1 << 8) | r2, 0, 0);
	frontend[channel].pot_r1 = r1;
	frontend[channel].pot_r2 = r2;
}

// --- Frontend state ---

JSONNode M1K_device::frontendChannelToJSON(unsigned ch) {
	JSONNode n(JSON_NODE);
	n.push_back(JSONNode("r50_2v5", frontend[ch].r50_2v5));
	n.push_back(JSONNode("r50_gnd", frontend[ch].r50_gnd));
	n.push_back(JSONNode("feedback", frontend[ch].feedback));
	n.push_back(JSONNode("output_en", frontend[ch].output_en));
	n.push_back(JSONNode("split", frontend[ch].split));

	JSONNode pot(JSON_ARRAY);
	pot.set_name("pot");
	pot.push_back(JSONNode("", frontend[ch].pot_r1));
	pot.push_back(JSONNode("", frontend[ch].pot_r2));
	n.push_back(pot);

	return n;
}

JSONNode M1K_device::frontendToJSON() {
	JSONNode n(JSON_NODE);
	JSONNode a = frontendChannelToJSON(0);
	a.set_name("a");
	JSONNode b = frontendChannelToJSON(1);
	b.set_name("b");
	n.push_back(a);
	n.push_back(b);
	return n;
}

void M1K_device::setFrontendSwitch(unsigned ch, const string& name, bool val) {
	// GPIO switches are active LOW: val=true means switch ON = pin LOW
	unsigned pin;
	if (name == "r50_2v5") {
		pin = ch == 0 ? M1K_CHA_50R_2V5 : M1K_CHB_50R_2V5;
		frontend[ch].r50_2v5 = val;
	} else if (name == "r50_gnd") {
		pin = ch == 0 ? M1K_CHA_50R_GND : M1K_CHB_50R_GND;
		frontend[ch].r50_gnd = val;
	} else if (name == "feedback") {
		pin = ch == 0 ? M1K_CHA_FEEDBACK : M1K_CHB_FEEDBACK;
		frontend[ch].feedback = val;
	} else if (name == "output_en") {
		pin = ch == 0 ? M1K_CHA_OUTPUT_EN : M1K_CHB_OUTPUT_EN;
		frontend[ch].output_en = val;
	} else if (name == "split") {
		pin = ch == 0 ? M1K_CHA_SPLIT : M1K_CHB_SPLIT;
		frontend[ch].split = val;
		// SPLIT pin is active HIGH (opposite of others)
		setGPIO(pin, val);
		return;
	} else {
		return;
	}
	// Active LOW switches: true = ON = pin LOW
	setGPIO(pin, !val);
}

// --- ADM1177 power monitoring ---

JSONNode M1K_device::readPower() {
	uint8_t buf[3];
	int r = controlTransfer(0xC0, 0x17, 0, 3, buf, 3);

	JSONNode n(JSON_NODE);
	if (r >= 1) {
		n.push_back(JSONNode("status_raw", (int)buf[0]));
		// Alert bit position depends on firmware version
		uint8_t alert_bit = (_fwversion >= "2.11") ? 0x8 : 0x4;
		n.push_back(JSONNode("alert_bit", (int)alert_bit));
		n.push_back(JSONNode("overcurrent", (buf[0] & alert_bit) != 0));
	} else {
		n.push_back(JSONNode("status_raw", 0));
		n.push_back(JSONNode("alert_bit", 0));
		n.push_back(JSONNode("overcurrent", false));
		n.push_back(JSONNode("error", r));
	}
	return n;
}

// --- Temperature ---

JSONNode M1K_device::readTemperature() {
	uint8_t buf[2];
	JSONNode n(JSON_NODE);

	int r = controlTransfer(0xC0, 0x19, 0, 0, buf, 2);
	if (r >= 2) {
		n.push_back(JSONNode("a", (int)((buf[0] << 8) | buf[1])));
	} else {
		n.push_back(JSONNode("a", 0));
	}

	r = controlTransfer(0xC0, 0x19, 1, 0, buf, 2);
	if (r >= 2) {
		n.push_back(JSONNode("b", (int)((buf[0] << 8) | buf[1])));
	} else {
		n.push_back(JSONNode("b", 0));
	}

	return n;
}

// --- LEDs ---

JSONNode M1K_device::ledToJSON() {
	JSONNode n(JSON_NODE);
	n.push_back(JSONNode("leds", (int)ledState));
	n.push_back(JSONNode("red", (ledState & 0x4) != 0));
	n.push_back(JSONNode("green", (ledState & 0x2) != 0));
	n.push_back(JSONNode("blue", (ledState & 0x1) != 0));
	return n;
}

void M1K_device::setLEDs(uint8_t state) {
	ledState = state & 0x7;
	controlTransfer(0x40, 0x03, ledState, 0, 0, 0);
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

	} else if (cmd == "getFrontend") {
		JSONNode reply(JSON_NODE);
		reply.push_back(JSONNode("_action", "return"));
		reply.push_back(JSONNode("id", jsonIntProp(n, "id", 0)));
		JSONNode fe = frontendToJSON();
		fe.set_name("frontend");
		reply.push_back(fe);
		client.sendJSON(reply);
		return true;

	} else if (cmd == "setFrontend") {
		string ch = jsonStringProp(n, "channel");
		unsigned ch_idx = (ch == "b" || ch == "B") ? 1 : 0;

		// Switch controls (optional)
		{ JSONNode::iterator i = n.find("r50_2v5");
		  if (i != n.end()) setFrontendSwitch(ch_idx, "r50_2v5", i->as_bool()); }
		{ JSONNode::iterator i = n.find("r50_gnd");
		  if (i != n.end()) setFrontendSwitch(ch_idx, "r50_gnd", i->as_bool()); }
		{ JSONNode::iterator i = n.find("feedback");
		  if (i != n.end()) setFrontendSwitch(ch_idx, "feedback", i->as_bool()); }
		{ JSONNode::iterator i = n.find("output_en");
		  if (i != n.end()) setFrontendSwitch(ch_idx, "output_en", i->as_bool()); }
		{ JSONNode::iterator i = n.find("split");
		  if (i != n.end()) setFrontendSwitch(ch_idx, "split", i->as_bool()); }

		// Digipot (optional)
		int pot_r1 = jsonIntProp(n, "pot_r1", -1);
		int pot_r2 = jsonIntProp(n, "pot_r2", -1);
		if (pot_r1 >= 0 || pot_r2 >= 0) {
			uint8_t r1 = (pot_r1 >= 0) ? (pot_r1 & 0x7f) : frontend[ch_idx].pot_r1;
			uint8_t r2 = (pot_r2 >= 0) ? (pot_r2 & 0x7f) : frontend[ch_idx].pot_r2;
			setDigipot(ch_idx, r1, r2);
		}

		JSONNode reply(JSON_NODE);
		reply.push_back(JSONNode("_action", "return"));
		reply.push_back(JSONNode("id", jsonIntProp(n, "id", 0)));
		JSONNode fe = frontendToJSON();
		fe.set_name("frontend");
		reply.push_back(fe);
		client.sendJSON(reply);
		return true;

	} else if (cmd == "getPower") {
		JSONNode reply(JSON_NODE);
		reply.push_back(JSONNode("_action", "return"));
		reply.push_back(JSONNode("id", jsonIntProp(n, "id", 0)));
		JSONNode pw = readPower();
		pw.set_name("power");
		reply.push_back(pw);
		client.sendJSON(reply);
		return true;

	} else if (cmd == "getTemperature") {
		JSONNode reply(JSON_NODE);
		reply.push_back(JSONNode("_action", "return"));
		reply.push_back(JSONNode("id", jsonIntProp(n, "id", 0)));
		JSONNode temp = readTemperature();
		temp.set_name("temperature");
		reply.push_back(temp);
		client.sendJSON(reply);
		return true;

	} else if (cmd == "getLED") {
		JSONNode reply(JSON_NODE);
		reply.push_back(JSONNode("_action", "return"));
		reply.push_back(JSONNode("id", jsonIntProp(n, "id", 0)));
		JSONNode led = ledToJSON();
		led.set_name("leds");
		reply.push_back(led);
		client.sendJSON(reply);
		return true;

	} else if (cmd == "setLED") {
		int val = jsonIntProp(n, "leds", -1);
		if (val >= 0) {
			setLEDs(val & 0x7);
		} else {
			// Build from individual booleans
			uint8_t state = ledState;
			{ JSONNode::iterator i = n.find("red");
			  if (i != n.end()) { if (i->as_bool()) state |= 0x4; else state &= ~0x4; } }
			{ JSONNode::iterator i = n.find("green");
			  if (i != n.end()) { if (i->as_bool()) state |= 0x2; else state &= ~0x2; } }
			{ JSONNode::iterator i = n.find("blue");
			  if (i != n.end()) { if (i->as_bool()) state |= 0x1; else state &= ~0x1; } }
			setLEDs(state);
		}

		JSONNode reply(JSON_NODE);
		reply.push_back(JSONNode("_action", "return"));
		reply.push_back(JSONNode("id", jsonIntProp(n, "id", 0)));
		JSONNode led = ledToJSON();
		led.set_name("leds");
		reply.push_back(led);
		client.sendJSON(reply);
		return true;

	} else if (cmd == "setSerial") {
		string newSerial = jsonStringProp(n, "serial", "");
		int r = -1;
		if (newSerial.size() > 0 && newSerial.size() <= 32) {
			r = controlTransfer(0x40, 0x05, 0, 0, (uint8_t*)newSerial.data(), newSerial.size());
		}

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
	if (!path.leaf() && path.matches("frontend")) {
		if (client->get_method() == "POST") {
			client->read_http_post_body(
				boost::bind(
					&M1K_device::handleRESTFrontendCallback,
					boost::static_pointer_cast<M1K_device>(shared_from_this()),
					client, _1));
		} else {
			JSONNode j = frontendToJSON();
			respondJSON(client, j);
		}
		return true;

	} else if (!path.leaf() && path.matches("power")) {
		JSONNode j = readPower();
		respondJSON(client, j);
		return true;

	} else if (!path.leaf() && path.matches("temperature")) {
		JSONNode j = readTemperature();
		respondJSON(client, j);
		return true;

	} else if (!path.leaf() && path.matches("leds")) {
		if (client->get_method() == "POST") {
			client->read_http_post_body(
				boost::bind(
					&M1K_device::handleRESTLEDCallback,
					boost::static_pointer_cast<M1K_device>(shared_from_this()),
					client, _1));
		} else {
			JSONNode j = ledToJSON();
			respondJSON(client, j);
		}
		return true;

	} else if (!path.leaf() && path.matches("calibration")) {
		if (client->get_method() == "POST") {
			client->read_http_post_body(
				boost::bind(
					&M1K_device::handleRESTCalibrationCallback,
					boost::static_pointer_cast<M1K_device>(shared_from_this()),
					client, _1));
		} else {
			JSONNode j(JSON_NODE);
			j.push_back(JSONNode("valid", cal.eeprom_valid == M1K_EEPROM_VALID));

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
			j.push_back(offsets);
			j.push_back(gp);
			j.push_back(gn);
			respondJSON(client, j);
		}
		return true;

	} else if (!path.leaf() && path.matches("serial")) {
		if (client->get_method() == "POST") {
			client->read_http_post_body(
				boost::bind(
					&M1K_device::handleRESTSerialCallback,
					boost::static_pointer_cast<M1K_device>(shared_from_this()),
					client, _1));
		} else {
			JSONNode j(JSON_NODE);
			j.push_back(JSONNode("serial", string(serial)));
			respondJSON(client, j);
		}
		return true;

	} else {
		return StreamingDevice::handleREST(path, client);
	}
}

// --- REST callbacks ---

void M1K_device::handleRESTFrontendCallback(session_ptr client, string postdata) {
	try {
		std::map<string, string> map;
		parse_query(postdata, map);

		string ch = map_get(map, "channel", "a");
		unsigned ch_idx = (ch == "b" || ch == "B") ? 1 : 0;

		string val;
		val = map_get(map, "r50_2v5", "");
		if (val.size()) setFrontendSwitch(ch_idx, "r50_2v5", val == "true" || val == "1");
		val = map_get(map, "r50_gnd", "");
		if (val.size()) setFrontendSwitch(ch_idx, "r50_gnd", val == "true" || val == "1");
		val = map_get(map, "feedback", "");
		if (val.size()) setFrontendSwitch(ch_idx, "feedback", val == "true" || val == "1");
		val = map_get(map, "output_en", "");
		if (val.size()) setFrontendSwitch(ch_idx, "output_en", val == "true" || val == "1");
		val = map_get(map, "split", "");
		if (val.size()) setFrontendSwitch(ch_idx, "split", val == "true" || val == "1");

		val = map_get(map, "pot_r1", "");
		string val2 = map_get(map, "pot_r2", "");
		if (val.size() || val2.size()) {
			uint8_t r1 = val.size() ? (atoi(val.c_str()) & 0x7f) : frontend[ch_idx].pot_r1;
			uint8_t r2 = val2.size() ? (atoi(val2.c_str()) & 0x7f) : frontend[ch_idx].pot_r2;
			setDigipot(ch_idx, r1, r2);
		}

		JSONNode j = frontendToJSON();
		respondJSON(client, j);
	} catch (std::exception& e) {
		respondError(client, e);
	}
}

void M1K_device::handleRESTLEDCallback(session_ptr client, string postdata) {
	try {
		std::map<string, string> map;
		parse_query(postdata, map);

		string raw = map_get(map, "leds", "");
		if (raw.size()) {
			setLEDs(atoi(raw.c_str()) & 0x7);
		} else {
			uint8_t state = ledState;
			string val;
			val = map_get(map, "red", "");
			if (val.size()) { if (val == "true" || val == "1") state |= 0x4; else state &= ~0x4; }
			val = map_get(map, "green", "");
			if (val.size()) { if (val == "true" || val == "1") state |= 0x2; else state &= ~0x2; }
			val = map_get(map, "blue", "");
			if (val.size()) { if (val == "true" || val == "1") state |= 0x1; else state &= ~0x1; }
			setLEDs(state);
		}

		JSONNode j = ledToJSON();
		respondJSON(client, j);
	} catch (std::exception& e) {
		respondError(client, e);
	}
}

void M1K_device::handleRESTCalibrationCallback(session_ptr client, string postdata) {
	try {
		JSONNode n = libjson::parse(postdata);

		JSONNode::iterator it = n.find("reset");
		if (it != n.end() && it->as_bool()) {
			for (int i = 0; i < 8; i++) {
				cal.offset[i] = 0.0;
				cal.gain_p[i] = 1.0;
				cal.gain_n[i] = 1.0;
			}
		} else {
			JSONNode j_off = n.at("offset");
			JSONNode j_gp = n.at("gain_p");
			JSONNode j_gn = n.at("gain_n");
			for (int i = 0; i < 8; i++) {
				cal.offset[i] = j_off[i].as_float();
				cal.gain_p[i] = j_gp[i].as_float();
				cal.gain_n[i] = j_gn[i].as_float();
			}
		}
		cal.eeprom_valid = M1K_EEPROM_VALID;
		int r = controlTransfer(0x40, 0x02, 0, 0, (uint8_t*)&cal, sizeof(cal));

		JSONNode j(JSON_NODE);
		j.push_back(JSONNode("status", r));
		j.push_back(JSONNode("valid", true));
		respondJSON(client, j);
	} catch (std::exception& e) {
		respondError(client, e);
	}
}

void M1K_device::handleRESTSerialCallback(session_ptr client, string postdata) {
	try {
		std::map<string, string> map;
		parse_query(postdata, map);
		string newSerial = map_get(map, "serial", "");

		int r = -1;
		if (newSerial.size() > 0 && newSerial.size() <= 32) {
			r = controlTransfer(0x40, 0x05, 0, 0, (uint8_t*)newSerial.data(), newSerial.size());
		}

		JSONNode j(JSON_NODE);
		j.push_back(JSONNode("status", r));
		j.push_back(JSONNode("serial", newSerial));
		respondJSON(client, j);
	} catch (std::exception& e) {
		respondError(client, e);
	}
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
