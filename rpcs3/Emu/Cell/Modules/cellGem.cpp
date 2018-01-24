#include "stdafx.h"
#include "Emu/IdManager.h"
#include "Emu/System.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Io/MouseHandler.h"

#include "cellGem.h"
#include "cellCamera.h"
#include "pad_thread.h"
#include "Utilities/Timer.h"

logs::channel cellGem("cellGem");

/**********************
 * HLE helper structs *
 **********************/

struct gem_t
{
	struct gem_color
	{
		float r, g, b;

		gem_color() : r(0.0f), g(0.0f), b(0.0f) {}
		gem_color(float r_, float g_, float b_)
		{
			r = clamp(r_);
			g = clamp(g_);
			b = clamp(b_);
		}

		float clamp(float f) const
		{
			return std::max(0.0f, std::min(f, 1.0f));
		}
	};

	struct gem_controller
	{
		u32 status;                     // connection status (CELL_GEM_STATUS_DISCONNECTED or CELL_GEM_STATUS_READY)
		u32 port;                       // assigned port
		bool enabled_magnetometer;      // whether the magnetometer is enabled (probably used for additional rotational precision)
		bool calibrated_magnetometer;   // whether the magnetometer is calibrated
		bool enabled_filtering;         // whether filtering is enabled
		u8 rumble;                      // rumble intensity
		gem_color sphere_rgb;           // RGB color of the sphere LED

		gem_controller() :
			status(CELL_GEM_STATUS_DISCONNECTED),
			enabled_filtering(false), rumble(0), sphere_rgb() {}
	};

	CellGemAttribute attribute;
	CellGemVideoConvertAttribute vc_attribute;
	u64 status_flags;
	bool enable_pitch_correction;

	std::array<gem_controller, CELL_GEM_MAX_NUM> controllers;
	u32 connected_controllers;

	Timer timer;

	// helper functions
	bool is_controller_ready(u32 gem_num) const
	{
		return controllers[gem_num].status == CELL_GEM_STATUS_READY;
	}

	void reset_controller(u32 gem_num)
	{
		switch (g_cfg.io.move)
		{
		default:
		case move_handler::null:
		{
			controllers[gem_num].status = CELL_GEM_STATUS_DISCONNECTED;
			controllers[gem_num].port = 0;

			connected_controllers = 0;
		}

		case move_handler::fake:
		{
			// fake 1 connected controller
			if (gem_num == 0)
			{
				controllers[gem_num].status = CELL_GEM_STATUS_READY;
				controllers[gem_num].port = 7u - gem_num;
			}
			else
			{
				controllers[gem_num].status = CELL_GEM_STATUS_DISCONNECTED;
				controllers[gem_num].port = 0;
			}

			connected_controllers = 1;
		}
		}
	}
};

/*************************
 * HLE helper functions  *
 *************************/

template <>
void fmt_class_string<move_handler>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto value)
	{
		switch (value)
		{
		case move_handler::null: return "Null";
		case move_handler::fake: return "Fake";
		}

		return unknown;
	});
}

/**
 * \brief Verifies that a Move controller id is valid
 * \param gem_num Move controler ID to verify
 * \return True if the ID is valid, false otherwise
 */
static bool check_gem_num(const u32 gem_num)
{
	return gem_num >= 0 && gem_num < CELL_GEM_MAX_NUM;
}


/**
* \brief Maps Move controller data (digital buttons, and analog Trigger data) to DS3 pad input.
*        Unavoidably some buttons are not common between Move and other DS3-mapped input devices.
*        TODO: Add mappings here once I decide on them.
*        NOTE: Perhaps button combos could be used to aleviate that (?)
 * \param port_no DS3 port number to use
 * \param digital_buttons Bitmask filled with CELL_GEM_CTRL_* values
 * \param analog_t Analog value of Move's Trigger. Currently mapped to R2/L2 (the max value)
 * \return true on success, false if port_no controller is invalid
 */
static bool map_to_ds3_input(const u32 port_no, be_t<u16>& digital_buttons, be_t<u16>& analog_t)
{
	const auto handler = fxm::get<pad_thread>();

	if (!handler)
	{
		return false;
	}

	auto& pads = handler->GetPads();

	const PadInfo& rinfo = handler->GetInfo();

	if (port_no >= rinfo.max_connect)
	{
		return false;
	}

	//We have a choice here of NO_DEVICE or READ_FAILED...lets try no device for now
	if (port_no >= rinfo.now_connect)
	{
		return false;
	}

	auto pad = pads[port_no];

	for (Button& button : pad->m_buttons)
	{
		//here we check btns, and set pad accordingly,
		//if something changed, set btnChanged

		if (button.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL1)
		{
			if (button.m_pressed) pad->m_digital_1 |= button.m_outKeyCode;
			else pad->m_digital_1 &= ~button.m_outKeyCode;

			switch (button.m_outKeyCode)
			{
			case CELL_PAD_CTRL_LEFT:
				pad->m_press_left = button.m_value;
				break;
			case CELL_PAD_CTRL_DOWN:
				pad->m_press_down = button.m_value;
				break;
			case CELL_PAD_CTRL_RIGHT:
				pad->m_press_right = button.m_value;
				break;
			case CELL_PAD_CTRL_UP:
				pad->m_press_up = button.m_value;
				break;
				//These arent pressure btns
			case CELL_PAD_CTRL_R3:
			case CELL_PAD_CTRL_L3:
			case CELL_PAD_CTRL_START:
			case CELL_PAD_CTRL_SELECT:
			default: break;
			}
		}
		else if (button.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL2)
		{
			if (button.m_pressed) pad->m_digital_2 |= button.m_outKeyCode;
			else pad->m_digital_2 &= ~button.m_outKeyCode;

			switch (button.m_outKeyCode)
			{
			case CELL_PAD_CTRL_SQUARE:
				pad->m_press_square = button.m_value;
				break;
			case CELL_PAD_CTRL_CROSS:
				pad->m_press_cross = button.m_value;
				break;
			case CELL_PAD_CTRL_CIRCLE:
				pad->m_press_circle = button.m_value;
				break;
			case CELL_PAD_CTRL_TRIANGLE:
				pad->m_press_triangle = button.m_value;
				break;
			case CELL_PAD_CTRL_R1:
				pad->m_press_R1 = button.m_value;
				break;
			case CELL_PAD_CTRL_L1:
				pad->m_press_L1 = button.m_value;
				break;
			case CELL_PAD_CTRL_R2:
				pad->m_press_R2 = button.m_value;
				break;
			case CELL_PAD_CTRL_L2:
				pad->m_press_L2 = button.m_value;
				break;
			default: break;
			}
		}

		if (button.m_flush)
		{
			button.m_pressed = false;
			button.m_flush = false;
			button.m_value = 0;
		}
	}

	memset(&digital_buttons, 0, sizeof(digital_buttons));

	// map the Move key to R1 and the trigger to L2 and R2
	if (pad->m_press_R1)
		digital_buttons |= CELL_GEM_CTRL_MOVE;
	if (pad->m_press_L2 || pad->m_press_R2)
		digital_buttons |= CELL_GEM_CTRL_T;

	if (pad->m_press_cross)
		digital_buttons |= CELL_GEM_CTRL_CROSS;
	if (pad->m_press_circle)
		digital_buttons |= CELL_GEM_CTRL_CIRCLE;
	if (pad->m_press_square)
		digital_buttons |= CELL_GEM_CTRL_SQUARE;
	if (pad->m_press_triangle)
		digital_buttons |= CELL_GEM_CTRL_TRIANGLE;
	if (pad->m_digital_1)
		digital_buttons |= CELL_GEM_CTRL_SELECT;
	if (pad->m_digital_2)
		digital_buttons |= CELL_GEM_CTRL_START;

	// map the Move Trigger to both L2 and R2 (whichever's most pressed)
	analog_t = std::max(pad->m_press_L2, pad->m_press_R2);

	return false;
}

/**
 * \brief Maps external Move controller data to DS3 input
 *	      Implementation detail: CellGemExtPortData's digital/analog fields map the same way as
 *	      libPad, so no translation is needed.
 * \param port_no DS3 port number to use
 * \param ext External data to modify
 * \return true on success, false if port_no controller is invalid
 */
static bool map_ext_to_ds3_input(const u32 port_no, CellGemExtPortData& ext)
{
	const auto handler = fxm::get<pad_thread>();

	if (!handler)
	{
		return false;
	}

	auto& pads = handler->GetPads();

	const PadInfo& rinfo = handler->GetInfo();

	if (port_no >= rinfo.max_connect)
	{
		return false;
	}

	//We have a choice here of NO_DEVICE or READ_FAILED...lets try no device for now
	if (port_no >= rinfo.now_connect)
	{
		return false;
	}

	auto pad = pads[port_no];

	ext.status = CELL_GEM_EXT_CONNECTED | CELL_GEM_EXT_EXT0 | CELL_GEM_EXT_EXT1; // report all! the ports
	ext.analog_left_x = pad->m_analog_left_x;
	ext.analog_left_y = pad->m_analog_left_y;
	ext.analog_right_x = pad->m_analog_right_x;
	ext.analog_right_y = pad->m_analog_right_y;
	ext.digital1 = pad->m_digital_1;
	ext.digital2 = pad->m_digital_2;

	return true;
}

/**
* \brief Maps Move controller data (digital buttons, and analog Trigger data) to mouse input.
*        Move Button: Mouse1
*        Trigger:     Mouse2
* \param mouse_no Mouse index number to use
* \param digital_buttons Bitmask filled with CELL_GEM_CTRL_* values
* \param analog_t Analog value of Move's Trigger.
* \return true on success, false if mouse mouse_no is invalid
*/
static bool map_to_mouse_input(const u32 mouse_no, be_t<u16>& digital_buttons, be_t<u16>& analog_t)
{
	const auto handler = fxm::get<MouseHandlerBase>();

	if (handler->GetInfo().status[mouse_no] != CELL_MOUSE_STATUS_CONNECTED)
	{
		return false;
	}

	const MouseData& mouse_data = handler->GetData(mouse_no);

	if (mouse_data.buttons & CELL_MOUSE_BUTTON_1)
		digital_buttons |= CELL_GEM_CTRL_T;

	if (mouse_data.buttons & CELL_MOUSE_BUTTON_2)
		digital_buttons |= CELL_GEM_CTRL_MOVE;

	analog_t = mouse_data.buttons & CELL_MOUSE_BUTTON_1 ? 0xFFFF : 0;

	return true;
}

/**********************
 * cellGem functions  *
 **********************/

s32 cellGemCalibrate(u32 gem_num)
{
	cellGem.todo("cellGemCalibrate(gem_num=%d)", gem_num);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (g_cfg.io.move == move_handler::fake)
	{
		gem->controllers[gem_num].calibrated_magnetometer = true;
		gem->status_flags = CELL_GEM_FLAG_CALIBRATION_OCCURRED |
							CELL_GEM_FLAG_CALIBRATION_SUCCEEDED;
	}

	// TODO: this is probably unnecessary
	std::this_thread::sleep_for(400ms);

	return CELL_OK;
}

s32 cellGemClearStatusFlags(u32 gem_num, u64 mask)
{
	cellGem.todo("cellGemClearStatusFlags(gem_num=%d, mask=0x%x)", gem_num, mask);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem->status_flags &= ~mask;

	return CELL_OK;
}

s32 cellGemConvertVideoFinish()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemConvertVideoStart()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemEnableCameraPitchAngleCorrection(u32 enable_flag)
{
	cellGem.todo("cellGemEnableCameraPitchAngleCorrection(enable_flag=%d", enable_flag);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	gem->enable_pitch_correction = enable_flag;

	return CELL_OK;
}

s32 cellGemEnableMagnetometer(u32 gem_num, u32 enable)
{
	cellGem.todo("cellGemEnableMagnetometer(gem_num=%d, enable=0x%x)", gem_num, enable);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!gem->is_controller_ready(gem_num))
	{
		return CELL_GEM_NOT_CONNECTED;
	}

	gem->controllers[gem_num].enabled_magnetometer = !!enable;

	return CELL_OK;
}

s32 cellGemEnd()
{
	cellGem.warning("cellGemEnd()");

	if (!fxm::remove<gem_t>())
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	fxm::remove<MouseHandlerBase>();

	return CELL_OK;
}

s32 cellGemFilterState(u32 gem_num, u32 enable)
{
	cellGem.warning("cellGemFilterState(gem_num=%d, enable=%d)", gem_num, enable);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem->controllers[gem_num].enabled_filtering = !!enable;

	return CELL_OK;
}

s32 cellGemForceRGB(u32 gem_num, float r, float g, float b)
{
	cellGem.todo("cellGemForceRGB(gem_num=%d, r=%f, g=%f, b=%f)", gem_num, r, g, b);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem->controllers[gem_num].sphere_rgb = gem_t::gem_color(r, g, b);

	return CELL_OK;
}

s32 cellGemGetAccelerometerPositionInDevice()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemGetAllTrackableHues(vm::ptr<u8> hues)
{
	cellGem.todo("cellGemGetAllTrackableHues(hues=*0x%x)");
	return CELL_OK;
}

s32 cellGemGetCameraState()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemGetEnvironmentLightingColor()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemGetHuePixels()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemGetImageState(u32 gem_num, vm::ptr<CellGemImageState> image_state)
{
	cellGem.todo("cellGemGetImageState(gem_num=%d, image_state=&0x%x)", gem_num, image_state);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	std::memset(image_state.get_ptr(), 0, sizeof(CellGemImageState));

	if (g_cfg.io.move == move_handler::fake &&
		g_cfg.io.mouse == mouse_handler::basic)
	{
		auto shared_data = fxm::get_always<gem_camera_shared>();

		const auto handler = fxm::get<MouseHandlerBase>();
		auto& mouse = handler->GetMice().at(0);

		f32 x_pos = mouse.x_pos;
		f32 y_pos = mouse.y_pos;

		// Only game this seems to work on is PAIN, others use different functions
		static constexpr auto aspect_ratio = 1.2;

		static constexpr auto screen_offset_x = 400.0;
		static constexpr auto screen_offset_y = screen_offset_x * aspect_ratio;

		static constexpr auto screen_scale = 3.0;

		image_state->frame_timestamp = shared_data->frame_timestamp.load();
		image_state->timestamp = image_state->frame_timestamp + 10;		// arbitrarily define 10 usecs of frame processing
		image_state->u = screen_offset_x / screen_scale + x_pos / screen_scale;
		image_state->v = screen_offset_y / screen_scale + y_pos / screen_scale * aspect_ratio;
		// below line is for debugging screen coords above
		// cellGem.error("xpos: %07.2f  ypos: %07.2f  u: %07.2f  v: %07.2f", x_pos, y_pos, image_state->u.value(), image_state->v.value());
		image_state->visible = true;
		image_state->r = 10;
		image_state->r_valid = true;
		image_state->distance = 2 * 1000;	// 2 meters away from camera
		// TODO
		image_state->projectionx = 1;
		image_state->projectiony = 1;
	}

	return CELL_OK;
}

s32 cellGemGetInertialState(u32 gem_num, u32 state_flag, u64 timestamp, vm::ptr<CellGemInertialState> inertial_state)
{
	cellGem.todo("cellGemGetInertialState(gem_num=%d, state_flag=%d, timestamp=0x%x, inertial_state=0x%x)", gem_num, state_flag, timestamp, inertial_state);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !inertial_state || !gem->is_controller_ready(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	std::memset(inertial_state.get_ptr(), 0, sizeof(CellGemInertialState));

	if (g_cfg.io.move == move_handler::fake)
	{
		map_to_ds3_input(gem_num, inertial_state->pad.digitalbuttons, inertial_state->pad.analog_T);
		map_ext_to_ds3_input(gem_num, inertial_state->ext);

		if (g_cfg.io.mouse == mouse_handler::basic)
		{
			map_to_mouse_input(gem_num, inertial_state->pad.digitalbuttons, inertial_state->pad.analog_T);
		}

		inertial_state->timestamp = gem->timer.GetElapsedTimeInMicroSec();
	}

	return CELL_OK;
}

s32 cellGemGetInfo(vm::ptr<CellGemInfo> info)
{
	cellGem.todo("cellGemGetInfo(info=*0x%x)", info);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!info)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	// TODO: Support connecting PlayStation Move controllers
	info->max_connect = gem->attribute.max_connect;
	info->now_connect = gem->connected_controllers;

	for (int i = 0; i < CELL_GEM_MAX_NUM; i++)
	{
		info->status[i] = gem->controllers[i].status;
		info->port[i] = gem->controllers[i].port;
	}

	return CELL_OK;
}

s32 cellGemGetMemorySize(s32 max_connect)
{
	cellGem.warning("cellGemGetMemorySize(max_connect=%d)", max_connect);

	if (max_connect > CELL_GEM_MAX_NUM || max_connect <= 0)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	return max_connect <= 2 ? 0x120000 : 0x140000;
}

s32 cellGemGetRGB(u32 gem_num, vm::ptr<float> r, vm::ptr<float> g, vm::ptr<float> b)
{
	cellGem.todo("cellGemGetRGB(gem_num=%d, r=*0x%x, g=*0x%x, b=*0x%x)", gem_num, r, g, b);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) | !r || !g || !b )
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	auto& sphere_color = gem->controllers[gem_num].sphere_rgb;
	*r = sphere_color.r;
	*g = sphere_color.g;
	*b = sphere_color.b;

	return CELL_OK;
}

s32 cellGemGetRumble(u32 gem_num, vm::ptr<u8> rumble)
{
	cellGem.todo("cellGemGetRumble(gem_num=%d, rumble=*0x%x)", gem_num, rumble);
	auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !rumble)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	*rumble = gem->controllers[gem_num].rumble;

	return CELL_OK;
}

s32 cellGemGetState(u32 gem_num, u32 flag, u64 time_parameter, vm::ptr<CellGemState> gem_state)
{
	cellGem.todo("cellGemGetState(gem_num=%d, flag=0x%x, time=0x%llx, gem_state=*0x%x)", gem_num, flag, time_parameter, gem_state);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (g_cfg.io.move == move_handler::fake)
	{
		map_to_ds3_input(gem_num, gem_state->pad.digitalbuttons, gem_state->pad.analog_T);
		map_ext_to_ds3_input(gem_num, gem_state->ext);

		if (g_cfg.io.mouse == mouse_handler::basic)
		{
			map_to_mouse_input(gem_num, gem_state->pad.digitalbuttons, gem_state->pad.analog_T);
		}

		gem_state->tracking_flags = CELL_GEM_TRACKING_FLAG_POSITION_TRACKED |
									CELL_GEM_TRACKING_FLAG_VISIBLE;
		gem_state->timestamp = gem->timer.GetElapsedTimeInMicroSec();
	}

	return CELL_GEM_NOT_CONNECTED;
}

s32 cellGemGetStatusFlags(u32 gem_num, vm::ptr<u64> flags)
{
	cellGem.todo("cellGemGetStatusFlags(gem_num=%d, flags=*0x%x)", gem_num, flags);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !flags)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	*flags = gem->status_flags;

	return CELL_OK;
}

s32 cellGemGetTrackerHue()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemHSVtoRGB()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemInit(vm::cptr<CellGemAttribute> attribute)
{
	cellGem.warning("cellGemInit(attribute=*0x%x)", attribute);

	const auto gem = fxm::make<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_ALREADY_INITIALIZED;
	}

	if (!attribute || !attribute->spurs_addr || attribute->max_connect > CELL_GEM_MAX_NUM)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem->attribute = *attribute;

	for (auto gem_num = 0; gem_num < CELL_GEM_MAX_NUM; gem_num++)
	{
		gem->reset_controller(gem_num);
	}

	if (g_cfg.io.move == move_handler::fake &&
		g_cfg.io.mouse == mouse_handler::basic)
	{
		// init mouse handler
		const auto handler = fxm::import_always<MouseHandlerBase>(Emu.GetCallbacks().get_mouse_handler);

		handler->Init(std::min(attribute->max_connect.value(), static_cast<u32>(CELL_GEM_MAX_NUM)));
	}

	// TODO: is this correct?
	gem->timer.Start();

	return CELL_OK;
}

s32 cellGemInvalidateCalibration()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemIsTrackableHue(u32 hue)
{
	cellGem.todo("cellGemIsTrackableHue(hue=%d)", hue);
	const auto gem = fxm::get<gem_t>();

	if (!gem || hue > 359)
	{
		return false;
	}

	return true;
}

s32 cellGemPrepareCamera()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemPrepareVideoConvert(vm::cptr<CellGemVideoConvertAttribute> vc_attribute)
{
	cellGem.todo("cellGemPrepareVideoConvert(vc_attribute=*0x%x)", vc_attribute);
	auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!vc_attribute)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	const auto vc = *vc_attribute;

	if (!vc_attribute || vc.version == 0 || vc.output_format == 0 ||
		vc.conversion_flags & CELL_GEM_VIDEO_CONVERT_UNK3 && !vc.buffer_memory)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (vc.video_data_out & 0x1f || vc.buffer_memory & 0xff)
	{
		return CELL_GEM_ERROR_INVALID_ALIGNMENT;
	}

	gem->vc_attribute = vc;

	return CELL_OK;
}

s32 cellGemReadExternalPortDeviceInfo()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemReset(u32 gem_num)
{
	cellGem.todo("cellGemReset(gem_num=%d)", gem_num);
	auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem->reset_controller(gem_num);

	// TODO: is this correct?
	gem->timer.Start();

	return CELL_OK;
}

s32 cellGemSetRumble(u32 gem_num, u8 rumble)
{
	cellGem.todo("cellGemSetRumble(gem_num=%d, rumble=0x%x)", gem_num, rumble);
	auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem->controllers[gem_num].rumble = rumble;

	return CELL_OK;
}

s32 cellGemSetYaw()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemTrackHues()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemUpdateFinish()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemUpdateStart(vm::cptr<void> camera_frame, u64 timestamp)
{
	cellGem.todo("cellGemUpdateStart(camera_frame=*0x%x, timestamp=%d)", camera_frame, timestamp);
	auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	return CELL_OK;
}

s32 cellGemWriteExternalPort()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

DECLARE(ppu_module_manager::cellGem)("libgem", []()
{
	REG_FUNC(libgem, cellGemCalibrate);
	REG_FUNC(libgem, cellGemClearStatusFlags);
	REG_FUNC(libgem, cellGemConvertVideoFinish);
	REG_FUNC(libgem, cellGemConvertVideoStart);
	REG_FUNC(libgem, cellGemEnableCameraPitchAngleCorrection);
	REG_FUNC(libgem, cellGemEnableMagnetometer);
	REG_FUNC(libgem, cellGemEnd);
	REG_FUNC(libgem, cellGemFilterState);
	REG_FUNC(libgem, cellGemForceRGB);
	REG_FUNC(libgem, cellGemGetAccelerometerPositionInDevice);
	REG_FUNC(libgem, cellGemGetAllTrackableHues);
	REG_FUNC(libgem, cellGemGetCameraState);
	REG_FUNC(libgem, cellGemGetEnvironmentLightingColor);
	REG_FUNC(libgem, cellGemGetHuePixels);
	REG_FUNC(libgem, cellGemGetImageState);
	REG_FUNC(libgem, cellGemGetInertialState);
	REG_FUNC(libgem, cellGemGetInfo);
	REG_FUNC(libgem, cellGemGetMemorySize);
	REG_FUNC(libgem, cellGemGetRGB);
	REG_FUNC(libgem, cellGemGetRumble);
	REG_FUNC(libgem, cellGemGetState);
	REG_FUNC(libgem, cellGemGetStatusFlags);
	REG_FUNC(libgem, cellGemGetTrackerHue);
	REG_FUNC(libgem, cellGemHSVtoRGB);
	REG_FUNC(libgem, cellGemInit);
	REG_FUNC(libgem, cellGemInvalidateCalibration);
	REG_FUNC(libgem, cellGemIsTrackableHue);
	REG_FUNC(libgem, cellGemPrepareCamera);
	REG_FUNC(libgem, cellGemPrepareVideoConvert);
	REG_FUNC(libgem, cellGemReadExternalPortDeviceInfo);
	REG_FUNC(libgem, cellGemReset);
	REG_FUNC(libgem, cellGemSetRumble);
	REG_FUNC(libgem, cellGemSetYaw);
	REG_FUNC(libgem, cellGemTrackHues);
	REG_FUNC(libgem, cellGemUpdateFinish);
	REG_FUNC(libgem, cellGemUpdateStart);
	REG_FUNC(libgem, cellGemWriteExternalPort);
});
