// Copyright 2011 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "WII_IPC_HLE.h"
#include "WII_IPC_HLE_Device.h"

class CWII_IPC_HLE_Device_usb_oh0 : public IWII_IPC_HLE_Device
{
public:
	CWII_IPC_HLE_Device_usb_oh0(u32 DeviceID, const std::string& DeviceName);

	virtual ~CWII_IPC_HLE_Device_usb_oh0();

	IPCCommandResult Open(u32 CommandAddress, u32 Mode) override;
	IPCCommandResult Close(u32 CommandAddress, bool Force) override;

	IPCCommandResult IOCtlV(u32 CommandAddress) override;
	IPCCommandResult IOCtl(u32 CommandAddress) override;

	void DoState(PointerWrap &p) override;

private:
	enum USBIOCtl
	{
		USBV0_IOCTL_CTRLMSG = 0,
		USBV0_IOCTL_BLKMSG = 1,
		USBV0_IOCTL_INTRMSG = 2,
		USBV0_IOCTL_SUSPENDDEV = 5,
		USBV0_IOCTL_RESUMEDEV = 6,
		USBV0_IOCTL_ISOMSG = 9,
		USBV0_IOCTL_GETDEVLIST = 12,
		USBV0_IOCTL_DEVREMOVALHOOK = 26,
		USBV0_IOCTL_DEVINSERTHOOK = 27,
		USBV0_IOCTL_DEVICECLASSCHANGE = 28,
		USBV0_IOCTL_DEVINSERTHOOKID = 30
	};
};

class CWII_IPC_HLE_Device_usb_oh0_57e_308 : public IWII_IPC_HLE_Device
{
public:
	CWII_IPC_HLE_Device_usb_oh0_57e_308(u32 DeviceID, const std::string& DeviceName);

	virtual ~CWII_IPC_HLE_Device_usb_oh0_57e_308();

	IPCCommandResult Open(u32 CommandAddress, u32 Mode) override;
	IPCCommandResult Close(u32 CommandAddress, bool Force) override;

	IPCCommandResult IOCtlV(u32 CommandAddress) override;
	IPCCommandResult IOCtl(u32 CommandAddress) override;

	void DoState(PointerWrap &p) override;

	s16 *stream_buffer;

	// Translated from register writes
	struct WSState
	{
		bool sample_on;
		bool mute;
		int freq;
		int gain;
		bool ec_reset;
		bool sp_on;
	};

	WSState sampler;

private:
	enum USBIOCtl
	{
		USBV0_IOCTL_CTRLMSG = 0,
		USBV0_IOCTL_BLKMSG = 1,
		USBV0_IOCTL_ISOMSG = 9,
		USBV0_IOCTL_DEVREMOVALHOOK = 26
	};

	enum USBEndpoint
	{
		AUDIO_OUT = 0x03,
		AUDIO_IN = 0x81,
		DATA_OUT = 0x02
	};

	struct USBSetupPacket
	{
		u8  bmRequestType;
		u8  bRequest;
		u16 wValue;
		u16 wIndex;
		u16 wLength;
	};

	enum Registers
	{
		SAMPLER_STATE = 0,
		SAMPLER_MUTE = 0xc0,

		SAMPLER_FREQ = 2,
		FREQ_8KHZ = 0,
		FREQ_11KHZ = 1,
		FREQ_RESERVED = 2,
		FREQ_16KHZ = 3, // default

		SAMPLER_GAIN = 4,
		GAIN_00dB = 0,
		GAIN_15dB = 1,
		GAIN_30dB = 2,
		GAIN_36dB = 3, // default

		EC_STATE = 0x14,

		SP_STATE = 0x38,
		SP_ENABLE = 0x1010,
		SP_SIN = 0x2001,
		SP_SOUT = 0x2004,
		SP_RIN = 0x200d
	};

	void SetRegister(const u32 cmd_ptr);
	void GetRegister(const u32 cmd_ptr) const;

	// Streaming input interface
	int pa_error; // PaError
	void *pa_stream; // PaStream

	void StreamLog(const char *msg);
	void StreamInit();
	void StreamTerminate();
	void StreamStart();
	void StreamStop();
	void StreamReadOne();
};

class CWII_IPC_HLE_Device_usb_oh0_46d_a03 : public IWII_IPC_HLE_Device
{
public:
	CWII_IPC_HLE_Device_usb_oh0_46d_a03(u32 DeviceID, const std::string& DeviceName);

	virtual ~CWII_IPC_HLE_Device_usb_oh0_46d_a03();

	IPCCommandResult Open(u32 CommandAddress, u32 Mode) override;
	IPCCommandResult Close(u32 CommandAddress, bool Force) override;

	IPCCommandResult IOCtlV(u32 CommandAddress) override;
	IPCCommandResult IOCtl(u32 CommandAddress) override;

	void DoState(PointerWrap &p) override;

	s16 *stream_buffer;

private:
	enum USBIOCtl
	{
		USBV0_IOCTL_CTRLMSG = 0,
		USBV0_IOCTL_BLKMSG = 1,
		USBV0_IOCTL_ISOMSG = 9,
		USBV0_IOCTL_DEVREMOVALHOOK = 26
	};

	enum USBEndpoint
	{
		AUDIO_IN = 0x84
	};

#pragma pack(push, 1)

	struct USBSetupPacket
	{
		u8  bmRequestType;
		u8  bRequest;
		u16 wValue;
		u16 wIndex;
		u16 wLength;
	};

	struct usb_configurationdesc
	{
		u8 bLength;
		u8 bDescriptorType;
		u16 wTotalLength;
		u8 bNumInterfaces;
		u8 bConfigurationValue;
		u8 iConfiguration;
		u8 bmAttributes;
		u8 bMaxPower;
	};

	struct usb_interfacedesc
	{
		u8 bLength;
		u8 bDescriptorType;
		u8 bInterfaceNumber;
		u8 bAlternateSetting;
		u8 bNumEndpoints;
		u8 bInterfaceClass;
		u8 bInterfaceSubClass;
		u8 bInterfaceProtocol;
		u8 iInterface;
	};

	struct usb_endpointdesc
	{
		u8 bLength;
		u8 bDescriptorType;
		u8 bEndpointAddress;
		u8 bmAttributes;
		u16 wMaxPacketSize;
		u8 bInterval;
		u8 bRefresh;
		u8 bSynchAddress;
	};

#pragma pack(pop)

};
