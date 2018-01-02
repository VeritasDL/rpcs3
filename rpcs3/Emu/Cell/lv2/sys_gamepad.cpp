#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"

#include "sys_gamepad.h"
#include "hidapi.h"

namespace vm { using namespace ps3; }

logs::channel sys_gamepad("sys_gamepad");

/**********************
 * HLE helper structs *
 **********************/

struct gem_lle_t
{
	hid_device *device = nullptr;
};

/*************************
 * HLE helper functions  *
 *************************/

#include <comdef.h>  // you will need this
void print_hid_error(hid_device* handle, const std::string func)
{
	const auto err = hid_error(handle);
	_bstr_t b(err);
	const char* c = b;
	sys_gamepad.error("%s error: %s", func, c);
}

void copy_out_data(vm::ps3::ptr<uint8_t>& out, const std::vector<u8>& data)
{
	std::memcpy(out.get_ptr(), data.data(), data.size());
}

void copy_out_data_buf(vm::ps3::ptr<uint8_t>& out, const u8* data, const size_t size)
{
	std::memcpy(out.get_ptr(), data, size);
}

/*******************
 * ycon functions  *
 *******************/

u32 sys_gamepad_ycon_initalize(vm::ps3::ptr<uint8_t> in, vm::ps3::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_initalize(in=%d, out=%d) -> CELL_OK", in, out);

	auto gem = fxm::make_always<gem_lle_t>();

	// Open the device using the VID and PID
	// gem->device = hid_open(0x054c, 0x03d5, NULL);
	// Open the device using the full path
	gem->device = hid_open_path(R"(\\?\hid#vid_054c&pid_03d5&col01#6&2a5e1280&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030})");

	if (!gem->device)
	{
		sys_gamepad.error("sys_gamepad_ycon_initalize: No Move controller detected.");
		return 1;
	}
	return CELL_OK;
}

u32 sys_gamepad_ycon_finalize(vm::ps3::ptr<uint8_t> in, vm::ps3::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_finalize(in=%d, out=%d) -> CELL_OK", in, out);
	auto gem = fxm::get<gem_lle_t>();

	if (gem)
		fxm::remove<gem_lle_t>();
	else
		sys_gamepad.warning("sys_gamepad_ycon_finalize called without being initialized");

	return CELL_OK;
}

u32 sys_gamepad_ycon_has_input_ownership(vm::ps3::ptr<uint8_t> in, vm::ps3::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_has_input_ownership(in=%d, out=%d) -> CELL_OK", in, out);

	*out = 1;

	// Return value seems to be ignored
	return CELL_OK;
}

u32 sys_gamepad_ycon_enumerate_device(vm::ps3::ptr<uint8_t> in, vm::ps3::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_enumerate_device(in=%d, out=%d) -> CELL_OK", in, out);

	// after the first four bytes, every 4 bytes chunkswe write to `out` gets passed to sys_gamepad_ycon_get_device_info's `in` arg

	std::vector<u8> data = {0, 0, 0, 1, 0, 0, 0, 1};
/*
	for (auto i = 0; i < 32; ++i)
		data.push_back(2 + i);
*/
	copy_out_data(out, data);

	return CELL_OK;
}

#define MOTION_FEATURE_REPORT_MAX_SIZE 0x1A27

u32 sys_gamepad_ycon_get_device_info(vm::ps3::ptr<uint8_t> in, vm::ps3::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_get_device_info(in=%d, out=%d) -> CELL_OK", in, out);

	*out = 1;

#if 0

#define MOTION_REPORT_0x02_SIZE 49
	struct motion_output_report_02 {
		u8 type, zero;
		u8 r, g, b;
		u8 zero2;
		u8 rumble;
	};

	if (in.addr())
		sys_gamepad.error("AYYYYYY IN %08X   %08X", *(be_t<u32>*)vm::base(in.addr()), *(u32*)vm::base(in.addr()));

	static auto aa = 0;

	int res;

	auto *buf = new u8[150];
	std::fill(buf, buf + 150, 0);

#define MAX_STR 255
#define printf_e sys_gamepad.error
	wchar_t wstr[MAX_STR];
	int i;

	// Enumerate and print the HID devices on the system
	struct hid_device_info *devs, *cur_dev;

	auto gem = fxm::get<gem_lle_t>();
	auto* handle = gem->device;


	/*
	if (handle)
	{
		memset(buf, 0, 65);

		auto a = out.get_ptr();

		//memcpy_s(&buf[1], 64, a, 64);

		const auto bytes_written = hid_write(handle, buf, 64);

		if (bytes_written != -1)
		{
			printf_e("written %d bytes", bytes_written);

			hid_set_nonblocking(handle, 0);

			res = hid_read_timeout(handle, buf, 64, 10);

			if (res != -1)
				printf_e("read %d bytes", res);
			else
				print_hid_error(handle, "hid_read_timeout");

			//return bytes_written;
		}
		else
		{
			print_hid_error(handle, "hid_write");
		}
	}
	*/

	auto iter = 0;
	if (handle && aa == 0)
	{
		++aa;
		for (auto ii = 0; ii < 0xff; ++ii) {
			// Read a Feature Report from the device
			memset(buf, 0, 65);

			buf[0] = ii;
			if (ii == 16 || ii == 17 || ii == 224)
				res = hid_get_feature_report(handle, buf, 150);
			else
				res = hid_get_feature_report(handle, buf, 65);

			// Print out the returned buffer.
			std::string st;

			st += fmt::format("Feature Report %d\n  ", ii);
			if (res == -1)
			{
				st += "error: ";
				const auto err = hid_error(handle);
				_bstr_t b(err);
				const char* c = b;
				st += c;
			}
			else
			{
				for (i = 0; i < res; i++)
					st += fmt::format("%02hhx ", buf[i]);
			}
			printf_e("%s", st);
		}

		// set LEDs
		struct motion_output_report_02 report02 = { 0 };

		report02.type = 0x02; /* set leds */
		report02.r = 128;
		report02.g = 10;
		report02.b = 230;

		//res = hid_write(handle, reinterpret_cast<const unsigned char *>(&report02), MOTION_REPORT_0x02_SIZE);
		//
		//if (res)
		//	printf_e("WROTE %d bytes", res);
		//else
		//{
		//	print_hid_error(handle, "write");
		//}
	}

	//	// Set the hid_read() function to be non-blocking.
	//	//hid_set_nonblocking(handle, 1);

	//	//// Send an Output report to toggle the LED (cmd 0x80)
	//	//buf[0] = 1; // First byte is report number
	//	//buf[1] = 0x80;
	//	//res = hid_write(handle, buf, 65);

	//	//// Send an Output report to request the state (cmd 0x81)
	//	//buf[1] = 0x81;
	//	//hid_write(handle, buf, 65);

	//	while (iter < 10)
	//	{
	//		++iter;
	//		printf_e("iteration %d", iter);
	//		// Read requested state
	//		res = hid_read_timeout(handle, buf, 65, 500);
	//		if (res < 0)
	//		{
	//			printf_e("Unable to read()");
	//			continue;
	//		}

	//		// Print out the returned buffer.
	//		for (i = 0; i < res; i++)
	//			printf_e("buf[%d]: %d", i, buf[i]);
	//	}

	//}

	//if (out.addr())
	//	sys_gamepad.error("AYYYYYY OUT %08X   %08X", *(be_t<u32>*)vm::base(out.addr()), *(u32*)vm::base(out.addr()));

#endif

	return CELL_OK;
}

u32 sys_gamepad_ycon_read_raw_report(vm::ps3::ptr<uint8_t> in, vm::ps3::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_read_raw_report(in=%d, out=%d) -> CELL_OK", in, out);

	auto gem = fxm::get<gem_lle_t>();

	auto *buf = new u8[65];
	std::fill(buf, buf + 65, 0);

	buf[0] = in[4];

	auto res = hid_get_feature_report(gem->device, buf, 64);	// HACK: 64

	if (res == -1)
		print_hid_error(gem->device, "hid_get_feature_report");

	copy_out_data_buf(out, buf + 1, 64 - 1);

	return CELL_OK;
}

u32 sys_gamepad_ycon_write_raw_report(vm::ps3::ptr<uint8_t> in, vm::ps3::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_write_raw_report(in=%d, out=%d) -> CELL_OK", in, out);

	auto gem = fxm::get<gem_lle_t>();

	auto *buf = new u8[65];
	std::fill(buf, buf + 65, 0);

	auto res = hid_send_feature_report(gem->device, in.get_ptr(), 64);	// HACK: 64

	if (res == -1)
		print_hid_error(gem->device, "sys_gamepad_ycon_write_raw_report");
	return CELL_OK;
}

u32 sys_gamepad_ycon_get_feature(vm::ps3::ptr<uint8_t> in, vm::ps3::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_get_feature(in=%d, out=%d) -> CELL_OK", in, out);

	auto gem = fxm::get<gem_lle_t>();

	auto *buf = new u8[65];
	std::fill(buf, buf + 65, 0);

	buf[0] = in[4];

	auto res = hid_get_feature_report(gem->device, buf, 64);	// HACK: 64

	if (res == -1)
		print_hid_error(gem->device, "hid_get_feature_report");

	copy_out_data_buf(out, buf + 1, 64 - 1);

	return CELL_OK;
}

u32 sys_gamepad_ycon_set_feature(vm::ps3::ptr<uint8_t> in, vm::ps3::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_set_feature(in=%d, out=%d) -> CELL_OK", in, out);

	auto gem = fxm::get<gem_lle_t>();

	auto *buf = new u8[65];
	std::fill(buf, buf + 65, 0);

	auto res = hid_send_feature_report(gem->device, in.get_ptr() + 4, 64 - 4);	// HACK: 64

	if (res == -1 )
		print_hid_error(gem->device, "send_hid_send_feature_report");

	return CELL_OK;
}

u32 sys_gamepad_ycon_is_gem(vm::ps3::ptr<uint8_t> in, vm::ps3::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_is_gem(in=%d, out=%d) -> CELL_OK", in, out);
	return CELL_OK;
}


// syscall(621,packet_id,uint8_t *in,uint8_t *out) Talk:LV2_Functions_and_Syscalls#Syscall_621_.280x26D.29 gamepad_if usage
u32 sys_gamepad_ycon_if(uint8_t packet_id, vm::ps3::ptr<uint8_t> in, vm::ps3::ptr<uint8_t> out)
{
	sys_gamepad.error("***       sys_gamepad_ycon_if( packet_ID= %d\tin= *0x%x\tout= *0x%x)", packet_id, in, out);
	switch (packet_id)
	{
	case 0:
		return sys_gamepad_ycon_initalize(in, out);
	case 1:
		return sys_gamepad_ycon_finalize(in, out);
	case 2:
		return sys_gamepad_ycon_has_input_ownership(in, out);
	case 3:
		return sys_gamepad_ycon_enumerate_device(in, out);
	case 4:
		return sys_gamepad_ycon_get_device_info(in, out);
	case 5:
		return sys_gamepad_ycon_read_raw_report(in, out);
	case 6:
		return sys_gamepad_ycon_write_raw_report(in, out);
	case 7:
		return sys_gamepad_ycon_get_feature(in, out);
	case 8:
		return sys_gamepad_ycon_set_feature(in, out);
	case 9:
		return sys_gamepad_ycon_is_gem(in, out);

	default:
		sys_gamepad.error("sys_gamepad_ycon_if(packet_id=*%d, in=%d, out=%d), unknown packet id", packet_id, in, out);
		break;
	}

	return CELL_OK;
}
