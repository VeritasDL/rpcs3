#include "stdafx.h"
#include "cellGem.h"

#include "cellCamera.h"
#include "Emu/IdManager.h"
#include "Emu/System.h"
#include "Emu/Cell/PPUModule.h"
#include "pad_thread.h"

#include "psmove.h"
#include "psmove_tracker.h"
#include "psmove_fusion.h"

#include <climits>

logs::channel cellGem("cellGem");

// **********************
// * HLE helper structs *
// **********************

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

		bool is_black() const
		{
			return r == 0.0f && g == 0.0f && b == 0.0f;
		}
	};

	struct PSMoveDeleter
	{
		void operator()(PSMove* p)
		{
			psmove_disconnect(p);
		}
	};
	struct PSMoveTrackerDeleter
	{
		void operator()(PSMoveTracker* p)
		{
			psmove_tracker_free(p);
		}
	};

	struct gem_controller
	{
		u32 status;                     // connection status (CELL_GEM_STATUS_DISCONNECTED or CELL_GEM_STATUS_READY)
		u32 ext_status;                 // external port connection status
		u32 port;                       // assigned port
		bool enabled_magnetometer;      // whether the magnetometer is enabled (probably used for additional rotational precision)
		bool calibrated_magnetometer;   // whether the magnetometer is calibrated
		bool enabled_filtering;         // whether filtering is enabled
		bool enabled_tracking;          // whether tracking is enabled
		bool enabled_LED;               // whether the LED is enabled
		u8 rumble;                      // rumble intensity
		gem_color sphere_rgb;           // RGB color of the sphere LED
		u32 hue;                        // tracking hue of the motion controller

		// PSMoveAPI per-controller data
		struct
		{
			std::unique_ptr<PSMove, PSMoveDeleter> handle;
			std::string serial;								// unique identifier (Bluetooth MAC address)
			bool enable_orientation;						// whether `psmove_enable_orientation` has been called and succeeded
		} psmove;

		gem_controller() :
			status(CELL_GEM_STATUS_DISCONNECTED),
			enabled_filtering(false), rumble(0), sphere_rgb() {}
	};

	// PSMoveAPI data
	struct
	{
		std::unique_ptr<PSMoveTracker, PSMoveTrackerDeleter> tracker;
		s32 connected_tracker_controllers;
	} psmove;

	CellGemAttribute attribute;
	CellGemVideoConvertAttribute vc_attribute;
	u64 status_flags;
	bool enable_pitch_correction;
	u32 inertial_counter;

	std::array<gem_controller, CELL_GEM_MAX_NUM> controllers;
	u32 connected_controllers;

	Timer timer;

	// helper functions
	bool is_controller_ready(u32 gem_num) const
	{
		return controllers[gem_num].status == CELL_GEM_STATUS_READY;
	}

	void reset_controller(gsl::not_null<gem_t*> gem, u32 gem_num);
};

// ************************
// * HLE helper functions *
// ************************

void gem_t::reset_controller(gsl::not_null<gem_t*> gem, u32 gem_num)
{
	switch (g_cfg.io.move)
	{
	default:
	case move_handler::null:
	{
		connected_controllers = 0;
		break;
	}
	case move_handler::fake:
	{
		connected_controllers = 1;
		break;
	}
	case move_handler::move:
	{
		connected_controllers = gem->connected_controllers;
		break;
	}
	}

	// Assign status and port number
	if (gem_num < connected_controllers)
	{
		controllers[gem_num].status = CELL_GEM_STATUS_READY;
		controllers[gem_num].port = 7u - gem_num;
	}
	else
	{
		controllers[gem_num].status = CELL_GEM_STATUS_DISCONNECTED;
		controllers[gem_num].port = 0;
	}
}

template <>
void fmt_class_string<move_handler>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto value)
	{
		switch (value)
		{
		case move_handler::null: return "Null";
		case move_handler::fake: return "Fake";
		case move_handler::move: return "PSMove";
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

std::pair<u32, u32> get_format_resolution(CellGemVideoConvertFormatEnum format)
{
	std::pair<u32, u32> res;
	switch (format)
	{
	case CELL_GEM_RGBA_640x480:
	case CELL_GEM_YUV_640x480:
	case CELL_GEM_YUV422_640x480:
	case CELL_GEM_YUV411_640x480:
	case CELL_GEM_BAYER_RESTORED:
		return{ 640, 480 };
	case CELL_GEM_RGBA_320x240:
	case CELL_GEM_BAYER_RESTORED_RGGB:
	case CELL_GEM_BAYER_RESTORED_RASTERIZED:
		return{ 320, 240 };
	case CELL_GEM_NO_VIDEO_OUTPUT:
	default:
		return{ 0, 0 };
	}
}

namespace move
{

namespace psmoveapi
{

static void init(gsl::not_null<gem_t*> gem)
{
	if (!psmove_init(PSMOVE_CURRENT_VERSION)) {
		fmt::throw_exception("Couldn't initialize PSMoveAPI");
		// TODO: Don't die
	}

	// const auto g_psmove = fxm::make<psmoveapi_thread>();

	gem->connected_controllers = psmove_count_connected();

	for (auto id = 0; id < gem->connected_controllers; ++id)
	{
		PSMove* controller_handle = psmove_connect_by_id(id);

		if (controller_handle)
		{
			const std::string serial = psmove_get_serial(controller_handle);

			auto& gem_controller = gem->controllers[id];

			gem_controller.psmove.serial = serial;
			gem_controller.psmove.handle.reset(controller_handle);

			// g_psmove->register_controller(id, connected_controller);

			psmove_set_orientation_fusion_type(controller_handle,
				PSMoveOrientation_Fusion_Type::OrientationFusion_ComplementaryMARG);
			psmove_enable_orientation(controller_handle, PSMove_True);
			gem_controller.psmove.enable_orientation = psmove_has_orientation(controller_handle);
		}
	}
}

							// TODO: In forcegrgb: if tracker: maybe do psmove_tracker_enable_with_color


namespace tracker
{
	static void init(gsl::not_null<gem_t*> gem)
	{
		PSMoveTrackerSettings settings;
		psmove_tracker_settings_set_default(&settings);
		settings.color_mapping_max_age = 0;
		settings.exposure_mode = Exposure_LOW;

		auto shared_data = fxm::get_always<gem_camera_shared>();

		settings.camera_mirror = static_cast<PSMove_Bool>(shared_data->attr.load()[CELL_CAMERA_MIRRORFLAG].v1);

		std::tie(settings.camera_frame_width, settings.camera_frame_height) =
			get_format_resolution(gem->vc_attribute.output_format);

		// settings.camera_frame_rate = gem->vc_attribute
		settings.camera_auto_white_balance = static_cast<PSMove_Bool>(gem->vc_attribute.conversion_flags & CELL_GEM_AUTO_WHITE_BALANCE);
		settings.camera_gain = gem->vc_attribute.gain;

		// TODO: settings

		PSMoveTracker* tracker = psmove_tracker_new_with_settings(&settings);
		if (!tracker)
		{
			fmt::throw_exception("Couldn't initialize PSMoveAPI Tracker");
			// TODO: Don't die
		}

		const auto connected = psmove_tracker_count_connected();
		gem->psmove.connected_tracker_controllers = connected;

		for (auto id = 0; id < connected; ++id)
		{
			auto* handle = gem->controllers[id].psmove.handle.get();
			if (handle)
			{
				psmove_tracker_set_auto_update_leds(tracker, handle, PSMove_True);
				psmove_tracker_set_exposure(tracker, PSMoveTracker_Exposure::Exposure_MEDIUM);
			}
		}
		gem->psmove.tracker.reset(tracker);
	}

	void enable(gsl::not_null<gem_t*> gem, u32 gem_num)
	{
		const auto& controller = gem->controllers[gem_num];
		const auto handle = controller.psmove.handle.get();
		const auto color = controller.sphere_rgb;

		PSMoveTracker_Status enable_status;

		// enable tracker for controller
		for (;;)
		{
			PSMoveTracker_Status tracker_status;

			if (color.is_black())
			{
				tracker_status = psmove_tracker_enable(gem->psmove.tracker.get(), handle);
			}
			else
			{
				tracker_status = psmove_tracker_enable_with_color(gem->psmove.tracker.get(), handle,
					color.r * 255, color.g * 255, color.b * 255);
			}

			if (tracker_status == Tracker_CALIBRATED)
			{
				LOG_ERROR(HLE, "PSMoveAPI: Calibrated");	// TODO: remove
				break;
			}
			switch (tracker_status)
			{
			case Tracker_NOT_CALIBRATED: LOG_ERROR(HLE, "PSMoveAPI: Controller not registered with tracker");  break;
			case Tracker_CALIBRATION_ERROR: LOG_ERROR(HLE, "PSMoveAPI: Calibration failed (check lighting, visibility)");  break;
			case Tracker_TRACKING: LOG_ERROR(HLE, "PSMoveAPI: Calibrated and successfully tracked in the camera");  break;
			default:;
			}
		}
	}

	void disable(gsl::not_null<gem_t*> gem, u32 gem_num)
	{
		const auto& handle = gem->controllers[gem_num].psmove.handle.get();

		psmove_tracker_disable(gem->psmove.tracker.get(), handle);
	}

	void update(gsl::not_null<gem_t*> gem)
	{
		const auto tracker = gem->psmove.tracker.get();

		// TODO: Run in cellCamera?
		psmove_tracker_update_image(tracker);

		const auto connected = gem->psmove.connected_tracker_controllers;
		for (auto id = 0; id < connected; ++id)
		{
			const auto& handle = gem->controllers[id].psmove.handle.get();

			psmove_tracker_update(tracker, handle);
		}
	}
} // namespace tracker

bool poll(PSMove* controller_handle)
{
	bool poll_success = false;

	// consume all buffered data
	while (psmove_poll(controller_handle)) {
		poll_success = true;
	}

	return poll_success;
}

bool poll(const gem_t::gem_controller& controller)
{
	return move::psmoveapi::poll(controller.psmove.handle.get());
}

void update_rumble(gem_t::gem_controller& controller)
{
	const auto  handle = controller.psmove.handle.get();

	psmove_set_rumble(handle, controller.rumble);
	psmove_update_leds(handle);

}

void update_color(const gem_t::gem_controller& controller)
{
	const auto color = controller.sphere_rgb;
	const auto handle = controller.psmove.handle.get();

	psmove_set_leds(handle, color.r * 255, color.g * 255, color.b * 255);
	psmove_update_leds(handle);
}

/*
void psmoveapi_thread::on_task()
{
	while (fxm::check<camera_thread>() && !Emu.IsStopped())
	{
		std::chrono::steady_clock::time_point frame_start = std::chrono::steady_clock::now();

		if (Emu.IsPaused())
		{
			std::this_thread::sleep_for(1ms);
			continue;
		}

		{
			semaphore_lock lock(mutex_poll);

			for (auto controller : m_controllers)
			{
				const auto handle = controller.second;

				move::psmoveapi::poll(handle);
			}
		}

		//std::this_thread::sleep_for(10ms);

		const std::chrono::microseconds frame_target_time{ static_cast<u32>(1000000.0 / 90) };

		std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

		std::chrono::microseconds frame_processing_time = std::chrono::duration_cast<std::chrono::microseconds>(now - frame_start);

		if (frame_processing_time < frame_target_time)
		{
			std::chrono::microseconds frame_idle_time = frame_target_time - frame_processing_time;
			std::this_thread::sleep_for(frame_idle_time);
		}
	}
}
*/

} // namespace psmoveapi

namespace map {

/**
 * \brief Maps Move controller data (digital buttons, and analog Trigger data) to DS3 pad input.
 *        Unavoidably buttons conflict with DS3 mappings, which is problematic for some games.
 * \param port_no DS3 port number to use
 * \param digital_buttons Bitmask filled with CELL_GEM_CTRL_* values
 * \param analog_t Analog value of Move's Trigger. Currently mapped to R2.
 * \return true on success, false if port_no controller is invalid
 */
static bool ds3_input_to_pad(const u32 port_no, be_t<u16>& digital_buttons, be_t<u16>& analog_t)
{
	const auto handler = fxm::get<pad_thread>();

	if (!handler)
	{
		return false;
	}

	const PadInfo& rinfo = handler->GetInfo();

	if (port_no >= rinfo.max_connect || port_no >= rinfo.now_connect)
	{
		return false;
	}

	auto& pads = handler->GetPads();
	auto pad = pads[port_no];

	for (Button& button : pad->m_buttons)
	{
		//here we check btns, and set pad accordingly,
		if (button.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL2)
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

	// map the Move key to R1 and the Trigger to R2

	if (pad->m_press_R1)
		digital_buttons |= CELL_GEM_CTRL_MOVE;
	if (pad->m_press_R2)
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

	analog_t = pad->m_press_R2;

	return true;
}

/**
 * \brief Maps external Move controller data to DS3 input
 *	      Implementation detail: CellGemExtPortData's digital/analog fields map the same way as
 *	      libPad, so no translation is needed.
 * \param port_no DS3 port number to use
 * \param ext External data to modify
 * \return true on success, false if port_no controller is invalid
 */
static bool ds3_input_to_ext(const u32 port_no, CellGemExtPortData& ext)
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

	ext.status = 0; // CELL_GEM_EXT_CONNECTED | CELL_GEM_EXT_EXT0 | CELL_GEM_EXT_EXT1
	ext.analog_left_x = pad->m_analog_left_x;
	ext.analog_left_y = pad->m_analog_left_y;
	ext.analog_right_x = pad->m_analog_right_x;
	ext.analog_right_y = pad->m_analog_right_y;
	ext.digital1 = pad->m_digital_1;
	ext.digital2 = pad->m_digital_2;

	return true;
}

static bool psmove_input_to_pad(const gem_t::gem_controller& controller, be_t<u16>& digital_buttons, be_t<u16>& analog_t)
{
	const auto handle = controller.psmove.handle.get();
	const auto buttons = psmove_get_buttons(handle);
	const auto trigger = psmove_get_trigger(handle);

	memset(&digital_buttons, 0, sizeof(digital_buttons));

	if (buttons & Btn_MOVE)
		digital_buttons |= CELL_GEM_CTRL_MOVE;
	if (buttons & Btn_T)
		digital_buttons |= CELL_GEM_CTRL_T;

	if (buttons & Btn_CROSS)
		digital_buttons |= CELL_GEM_CTRL_CROSS;
	if (buttons & Btn_CIRCLE)
		digital_buttons |= CELL_GEM_CTRL_CIRCLE;
	if (buttons & Btn_SQUARE)
		digital_buttons |= CELL_GEM_CTRL_SQUARE;
	if (buttons & Btn_TRIANGLE)
		digital_buttons |= CELL_GEM_CTRL_TRIANGLE;
	if (buttons & Btn_SELECT)
		digital_buttons |= CELL_GEM_CTRL_SELECT;
	if (buttons & Btn_START)
		digital_buttons |= CELL_GEM_CTRL_START;

	analog_t = trigger * float(USHRT_MAX) / float(UCHAR_MAX);

	return true;
}
static bool psmove_input_to_gem(const gem_t::gem_controller& controller, vm::ptr<CellGemState>& gem_state)
{
	const auto handle = controller.psmove.handle.get();

	if (!psmove_has_calibration(handle))
	{
		fmt::throw_exception("You need to calibrate your Move Motion controller!");
	}

	float w, x, y, z;
	psmove_get_orientation(handle, &w, &x, &y, &z);
	gem_state->quat[0] = x;
	gem_state->quat[1] = y;
	gem_state->quat[2] = z;
	gem_state->quat[3] = w;

	// gem_state->

	return false;
}

static bool psmove_input_to_inertial(const gem_t::gem_controller& controller, vm::ptr<CellGemInertialState>& inertial_state)
{
	const auto handle = controller.psmove.handle.get();

	if (!psmove_has_calibration(handle))
	{
		fmt::throw_exception("You need to calibrate your Move Motion controller!");
	}

	float ax, ay, az;
	psmove_get_accelerometer_frame(handle, Frame_SecondHalf, &ax, &ay, &az);
	inertial_state->accelerometer[0] = ax;
	inertial_state->accelerometer[1] = az;
	inertial_state->accelerometer[2] = ay;
	inertial_state->accelerometer[3] = 0;

	float gx, gy, gz;
	psmove_get_gyroscope_frame(handle, Frame_SecondHalf, &gx, &gy, &gz);
	inertial_state->gyro[0] = gx;
	inertial_state->gyro[1] = gz;
	inertial_state->gyro[2] = gy;
	inertial_state->gyro[3] = 0;

	auto ac_b = inertial_state->accelerometer_bias;
	ac_b[0] = ac_b[1] = ac_b[2] = ac_b[3] = 0;

	auto gr_b = inertial_state->gyro_bias;
	gr_b[0] = gr_b[1] = gr_b[2] = gr_b[3] = 0;

	//LOG_FATAL(GENERAL, "Accel: { x: %+01.3f y: %+01.3f z: %+01.3f w: %+01.3f }  Gyro: { x: %+01.3f y: %+01.3f z: %+01.3f w: %+01.3f }  ",
	//	inertial_state->accelerometer[0], inertial_state->accelerometer[1], inertial_state->accelerometer[2], inertial_state->accelerometer[3],
	//	inertial_state->gyro[0], inertial_state->gyro[1], inertial_state->gyro[2], inertial_state->gyro[3]);

	return false;
}

} // namespace map
} // namespace move

// *********************
// * cellGem functions *
// *********************

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

	// if (g_cfg.io.move == move_handler::fake)
	{
		gem->controllers[gem_num].calibrated_magnetometer = true;
		gem->status_flags = CELL_GEM_FLAG_CALIBRATION_OCCURRED | CELL_GEM_FLAG_CALIBRATION_SUCCEEDED;
	}

	if (g_cfg.io.move == move_handler::move)
	{
		move::psmoveapi::tracker::enable(gem.get(), gem_num);
	}

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
	cellGem.todo("cellGemConvertVideoFinish()");
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	return CELL_OK;
}

s32 cellGemConvertVideoStart(vm::cptr<void> video_frame)
{
	cellGem.todo("cellGemConvertVideoStart(video_frame=*0x%x)", video_frame);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	return CELL_OK;
}

s32 cellGemEnableCameraPitchAngleCorrection(u32 enable_flag)
{
	cellGem.todo("cellGemEnableCameraPitchAngleCorrection(enable_flag=%d)", enable_flag);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	gem->enable_pitch_correction = !!enable_flag;

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

	auto& controller = gem->controllers[gem_num];
	controller.sphere_rgb = gem_t::gem_color(r, g, b);

	if (g_cfg.io.move == move_handler::move)
	{
		move::psmoveapi::update_color(controller);
	}

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
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!hues)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	for (size_t i = 0; i < 360; i++)
	{
		hues[i] = true;
	}

	return CELL_OK;
}

s32 cellGemGetCameraState(vm::ptr<CellGemCameraState> camera_state)
{
	cellGem.todo("cellGemGetCameraState(camera_state=0x%x)", camera_state);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!camera_state)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	camera_state->exposure_time = 1.0f / 60.0f;	// TODO: use correct framerate
	camera_state->gain = 1.0;

	return CELL_OK;
}

s32 cellGemGetEnvironmentLightingColor(vm::ptr<f32> r, vm::ptr<f32> g, vm::ptr<f32> b)
{
	cellGem.todo("cellGemGetEnvironmentLightingColor(r=*0x%x, g=*0x%x, b=*0x%x)", r, g, b);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!r || !g || !b)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	// default to 128
	*r = 128;
	*g = 128;
	*b = 128;

	return CELL_OK;
}

s32 cellGemGetHuePixels(vm::cptr<void> camera_frame, u32 hue, vm::ptr<u8> pixels)
{
	cellGem.todo("cellGemGetHuePixels(camera_frame=*0x%x, hue=%d, pixels=*0x%x)", camera_frame, hue, pixels);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!camera_frame || !pixels || hue > 359)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

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

	if (!check_gem_num(gem_num) || !image_state)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (g_cfg.io.move == move_handler::fake)
	{
		auto shared_data = fxm::get_always<gem_camera_shared>();

		image_state->frame_timestamp = shared_data->frame_timestamp.load();
		image_state->timestamp = image_state->frame_timestamp + 10;   // arbitrarily define 10 usecs of frame processing
		image_state->visible = true;
		image_state->u = 0;
		image_state->v = 0;
		image_state->r = 20;
		image_state->r_valid = true;
		image_state->distance = 2 * 1000;   // 2 meters away from camera
		// TODO
		image_state->projectionx = 1;
		image_state->projectiony = 1;
	}
	else if (g_cfg.io.move == move_handler::move)
	{
		const auto shared_data = fxm::get_always<gem_camera_shared>();
		const auto tracker = gem->psmove.tracker.get();
		const auto handle = gem->controllers[gem_num].psmove.handle.get();

		s32 width, height;
		psmove_tracker_get_size(tracker, &width, &height);

		float x, y, radius;
		const auto age = psmove_tracker_get_position(tracker, handle, &x, &y, &radius) * 1000;  // ms -> us
		const auto distance = psmove_tracker_distance_from_radius(tracker, radius) * 10;  // cm -> mm

		image_state->frame_timestamp = shared_data->frame_timestamp.load();
		image_state->timestamp = image_state->frame_timestamp + age;
		image_state->visible = true;
		image_state->u = x  * width;
		image_state->v = y * height;
		image_state->r = radius;
		image_state->r_valid = true;
		image_state->distance = distance;
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

	if (!check_gem_num(gem_num) || state_flag > CELL_GEM_INERTIAL_STATE_FLAG_NEXT || !inertial_state || !gem->is_controller_ready(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	// TODO(velocity): Abstract with gem state func
	if (g_cfg.io.move == move_handler::fake)
	{
		move::map::ds3_input_to_pad(gem_num, inertial_state->pad.digitalbuttons, inertial_state->pad.analog_T);
		move::map::ds3_input_to_ext(gem_num, inertial_state->ext);
	}
	else if (g_cfg.io.move == move_handler::move)
	{
		auto& handle = gem->controllers[gem_num];

		move::psmoveapi::poll(handle);

		move::map::psmove_input_to_pad(handle, inertial_state->pad.digitalbuttons, inertial_state->pad.analog_T);
		move::map::psmove_input_to_inertial(handle, inertial_state);
	}

	// TODO: handle timestamp arg by storing previous states

	// TODO: should this be in if above?
	inertial_state->timestamp = gem->timer.GetElapsedTimeInMicroSec();
	inertial_state->counter = gem->inertial_counter++;

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

	if (!check_gem_num(gem_num) || !r || !g || !b )
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

	if (!check_gem_num(gem_num) || flag > CELL_GEM_STATE_FLAG_TIMESTAMP || !gem_state)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (g_cfg.io.move == move_handler::fake)
	{
		move::map::ds3_input_to_pad(gem_num, gem_state->pad.digitalbuttons, gem_state->pad.analog_T);
		move::map::ds3_input_to_ext(gem_num, gem_state->ext);

		gem_state->tracking_flags = CELL_GEM_TRACKING_FLAG_POSITION_TRACKED | CELL_GEM_TRACKING_FLAG_VISIBLE;
	}
	else if (g_cfg.io.move == move_handler::move)
	{
		auto& handle = gem->controllers[gem_num];

		move::psmoveapi::poll(handle);

		move::map::psmove_input_to_pad(handle, gem_state->pad.digitalbuttons, gem_state->pad.analog_T);
		move::map::psmove_input_to_gem(handle, gem_state);

		// TODO(velocity)
		gem_state->tracking_flags = CELL_GEM_TRACKING_FLAG_POSITION_TRACKED | CELL_GEM_TRACKING_FLAG_VISIBLE;
	}

	if (g_cfg.io.move == move_handler::fake || g_cfg.io.move == move_handler::move)
	{
		gem_state->timestamp = gem->timer.GetElapsedTimeInMicroSec();
		gem_state->quat[3] = 1.0;

		return CELL_OK;
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

s32 cellGemGetTrackerHue(u32 gem_num, vm::ptr<u32> hue)
{
	cellGem.warning("cellGemGetTrackerHue(gem_num=%d, hue=*0x%x)", gem_num, hue);
	const auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !hue)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!gem->controllers[gem_num].enabled_tracking || gem->controllers[gem_num].hue > 359)
	{
		return CELL_GEM_ERROR_NOT_A_HUE;
	}

	*hue = gem->controllers[gem_num].hue;

	return CELL_OK;
}

s32 cellGemHSVtoRGB(f32 h, f32 s, f32 v, vm::ptr<f32> r, vm::ptr<f32> g, vm::ptr<f32> b)
{
	cellGem.todo("cellGemHSVtoRGB(h=%f, s=%f, v=%f, r=*0x%x, g=*0x%x, b=*0x%x)", h, s, v, r, g, b);

	if (s < 0.0f || s > 1.0f || v < 0.0f || v > 1.0f || !r || !g || !b)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	h = std::clamp(h, 0.0f, 360.0f);

	// TODO: convert

	return CELL_OK;
}

s32 cellGemInit(vm::cptr<CellGemAttribute> attribute)
{
	cellGem.warning("cellGemInit(attribute=*0x%x)", attribute);

	const auto gem = fxm::make_always<gem_t>();
	//const auto gem = fxm::make<gem_t>();

	//if (!gem)
	//{
	//	return CELL_GEM_ERROR_ALREADY_INITIALIZED;
	//}

	if (!attribute || !attribute->spurs_addr || attribute->max_connect > CELL_GEM_MAX_NUM)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem->attribute = *attribute;

	if (g_cfg.io.move == move_handler::move)
	{
		// Initialize psmoveapi
		move::psmoveapi::init(gem.get());
	}

	for (auto gem_num = 0; gem_num < CELL_GEM_MAX_NUM; gem_num++)
	{
		gem->reset_controller(gem.get(), gem_num);
	}

	// TODO: is this correct?
	gem->timer.Start();

	return CELL_OK;
}

s32 cellGemInvalidateCalibration(s32 gem_num)
{
	cellGem.todo("cellGemInvalidateCalibration(gem_num=%d)", gem_num);
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
		gem->controllers[gem_num].calibrated_magnetometer = false;
		// TODO: gem->status_flags
	}

	if (g_cfg.io.move == move_handler::move)
	{
		move::psmoveapi::tracker::disable(gem.get(), gem_num);
	}

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

s32 cellGemPrepareCamera(s32 max_exposure, f32 image_quality)
{
	cellGem.todo("cellGemPrepareCamera(max_exposure=%d, image_quality=%f)", max_exposure, image_quality);
	auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	max_exposure = std::clamp(max_exposure, static_cast<s32>(CELL_GEM_MIN_CAMERA_EXPOSURE), static_cast<s32>(CELL_GEM_MAX_CAMERA_EXPOSURE));
	image_quality = std::clamp(image_quality, 0.0f, 1.0f);

	// TODO: prepare camera

	if (g_cfg.io.move == move_handler::move)
	{
		move::psmoveapi::tracker::init(gem.get());
	}

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
	gem->vc_attribute = vc;

	if (!vc_attribute || vc.version == 0 || vc.output_format == 0 ||
		vc.conversion_flags & CELL_GEM_COMBINE_PREVIOUS_INPUT_FRAME && !vc.buffer_memory)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (vc.video_data_out & 0x1f || vc.buffer_memory & 0xff)
	{
		return CELL_GEM_ERROR_INVALID_ALIGNMENT;
	}

	return CELL_OK;
}

s32 cellGemReadExternalPortDeviceInfo(u32 gem_num, vm::ptr<u32> ext_id, vm::ptr<u8[CELL_GEM_EXTERNAL_PORT_DEVICE_INFO_SIZE]> ext_info)
{
	cellGem.todo("cellGemReset(gem_num=%d, ext_id=*0x%x, ext_info=%s)", gem_num, ext_id, ext_info);
	auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !ext_id)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (gem->controllers[gem_num].status & CELL_GEM_STATUS_DISCONNECTED)
	{
		return CELL_GEM_NOT_CONNECTED;
	}

	if (!(gem->controllers[gem_num].ext_status & CELL_GEM_EXT_CONNECTED))
	{
		return CELL_GEM_NO_EXTERNAL_PORT_DEVICE;
	}

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

	gem->reset_controller(gem.get(), gem_num);

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

	auto& controller = gem->controllers[gem_num];
	controller.rumble = rumble;

	if (g_cfg.io.move == move_handler::move)
	{
		move::psmoveapi::update_rumble(controller);
	}

	return CELL_OK;
}

s32 cellGemSetYaw()
{
	UNIMPLEMENTED_FUNC(cellGem);
	return CELL_OK;
}

s32 cellGemTrackHues(vm::cptr<u32> req_hues, vm::ptr<u32> res_hues)
{
	cellGem.todo("cellGemTrackHues(req_hues=*0x%x, res_hues=*0x%x)", req_hues, res_hues);
	auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!req_hues)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	for (size_t i = 0; i < CELL_GEM_MAX_NUM; i++)
	{
		if (req_hues[i] == CELL_GEM_DONT_CARE_HUE)
		{

		}
		else if (req_hues[i] == CELL_GEM_DONT_TRACK_HUE)
		{
			gem->controllers[i].enabled_tracking = false;
			gem->controllers[i].enabled_LED = false;
		}
		else
		{
			if (req_hues[i] > 359)
			{
				cellGem.warning("cellGemTrackHues: req_hues[%d]=%d -> this can lead to unexpected behavior", i, req_hues[i]);
			}
		}
	}

	return CELL_OK;
}

s32 cellGemUpdateFinish()
{
	cellGem.todo("cellGemUpdateFinish()");
	auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (g_cfg.io.move == move_handler::move)
	{
		move::psmoveapi::tracker::update(gem.get());
	}

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

	if (!camera_frame)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	return CELL_OK;
}

s32 cellGemWriteExternalPort(u32 gem_num, vm::ptr<u8[CELL_GEM_EXTERNAL_PORT_OUTPUT_SIZE]> data)
{
	cellGem.todo("cellGemWriteExternalPort(gem_num=%d, data=%s)", gem_num, data);
	auto gem = fxm::get<gem_t>();

	if (!gem)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

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
