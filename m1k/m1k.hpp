// Nonolith Connect
// M1K (ADALM1000) device support
// Released under the terms of the GNU GPLv3+
// Authors:
//   Based on CEE device by Kevin Mehall <km@kevinmehall.net>

#pragma once

#include "../dataserver.hpp"
#include "../streaming_device/streaming_device.hpp"
#include "../usb_device.hpp"
#include <boost/thread/mutex.hpp>

// EEPROM calibration format (matches libsmu device_m1000.hpp)
#define M1K_EEPROM_VALID 0x01ee02dd

struct M1K_EEPROM_cal {
	uint32_t eeprom_valid;
	float offset[8];
	float gain_p[8];
	float gain_n[8];
} __attribute__((packed));

// Calibration index mapping:
//   0: ChA measure voltage    1: ChA measure current
//   2: ChA source voltage     3: ChA source current
//   4: ChB measure voltage    5: ChB measure current
//   6: ChB source voltage     7: ChB source current

#define M1K_CHUNK_SIZE       256
#define M1K_IN_PACKET_SIZE   (M1K_CHUNK_SIZE * 4 * 2)   // 2048 bytes
#define M1K_OUT_PACKET_SIZE  (M1K_CHUNK_SIZE * 2 * 2)    // 1024 bytes

#define M1K_TIMER_CLOCK      48e6
#define M1K_MIN_PER          240     // 100 ksps
#define M1K_MAX_PER          24000   // ~1 ksps

#define M1K_N_TRANSFERS      4

// Voltage/current ADC conversion constants (from libsmu)
#define M1K_V_RESOLUTION     (5.0 / 65536.0)
#define M1K_I_RESOLUTION     (0.4 / 65536.0)
#define M1K_V_MIN            0.0
#define M1K_V_MAX            5.0
#define M1K_I_MIN            -200.0   // mA
#define M1K_I_MAX            200.0    // mA

// GPIO pin definitions (PIOB offset = 0x20)
#define M1K_CHA_50R_2V5     (0x20 + 0)   // PB0
#define M1K_CHA_50R_GND     (0x20 + 1)   // PB1
#define M1K_CHA_FEEDBACK    (0x20 + 2)   // PB2
#define M1K_CHA_OUTPUT_EN   (0x20 + 3)   // PB3
#define M1K_CHB_50R_2V5     (0x20 + 5)   // PB5
#define M1K_CHB_50R_GND     (0x20 + 6)   // PB6
#define M1K_CHB_FEEDBACK    (0x20 + 7)   // PB7
#define M1K_CHB_OUTPUT_EN   (0x20 + 8)   // PB8
#define M1K_CHA_SPLIT       34            // PA2
#define M1K_CHB_SPLIT       39            // PA7

enum M1K_mode {
	M1K_HI_Z = 0,
	M1K_SVMI = 1,
	M1K_SIMV = 2,
	M1K_HI_Z_SPLIT = 3,
	M1K_SVMI_SPLIT = 4,
	M1K_SIMV_SPLIT = 5,
};

// Per-channel analog front-end state (tracked locally)
struct M1K_frontend {
	bool r50_2v5;
	bool r50_gnd;
	bool feedback;
	bool output_en;
	bool split;
	uint8_t pot_r1;
	uint8_t pot_r2;
};

class M1K_device: public StreamingDevice, USB_device {
public:
	M1K_device(libusb_device *dev, libusb_device_descriptor &desc);
	virtual ~M1K_device();

	virtual void configure(int mode, double sampleTime, unsigned samples, bool continuous, bool raw);

	virtual const string model() { return "com.analogdevices.m1k"; }
	virtual const string hwVersion() { return _hwversion; }
	virtual const string fwVersion() { return _fwversion; }
	virtual const string serialno() { return serial; }

	virtual bool processMessage(ClientConn& session, string& cmd, JSONNode& n);
	virtual bool handleREST(UrlPath path, session_ptr client);

	virtual void setOutput(Channel* channel, OutputSource* source);

	libusb_transfer* in_transfers[M1K_N_TRANSFERS];
	libusb_transfer* out_transfers[M1K_N_TRANSFERS];

	Channel channel_a;
	Channel channel_b;

	Stream channel_a_v;
	Stream channel_a_i;
	Stream channel_b_v;
	Stream channel_b_i;

	boost::mutex outputMutex;
	boost::mutex transfersMutex;

	void fillOutTransfer(unsigned char* buf);
	void handleInTransfer(unsigned char* buf);

	unsigned incount, outcount;
	int packets_per_transfer;

protected:
	string _hwversion, _fwversion;

	virtual void on_reset_capture();
	virtual void on_start_capture();
	virtual void on_pause_capture();

	uint16_t encode_out(unsigned channel, float val);
	void checkOutputEffective(Channel& channel);
	void set_mode(unsigned channel, unsigned mode);

	void readCalibration();

	// GPIO helpers
	void setGPIO(unsigned pin, bool high);
	void setDigipot(unsigned channel, uint8_t r1, uint8_t r2);

	// Frontend state query/modify
	JSONNode frontendToJSON();
	JSONNode frontendChannelToJSON(unsigned ch);
	void setFrontendSwitch(unsigned ch, const string& name, bool val);

	// ADM1177 power monitoring
	JSONNode readPower();

	// Temperature
	JSONNode readTemperature();

	// LED state
	JSONNode ledToJSON();
	void setLEDs(uint8_t state);

	M1K_EEPROM_cal cal;
	unsigned m_mode[2];
	int m1k_per;
	uint16_t sof_start;
	uint8_t ledState;
	bool fw_interleaved; // true for fw >= 2.0 (interleaved data format)
	M1K_frontend frontend[2]; // [0]=A, [1]=B

	// REST callbacks
	void handleRESTFrontendCallback(session_ptr client, string postdata);
	void handleRESTLEDCallback(session_ptr client, string postdata);
	void handleRESTCalibrationCallback(session_ptr client, string postdata);
	void handleRESTSerialCallback(session_ptr client, string postdata);
};
