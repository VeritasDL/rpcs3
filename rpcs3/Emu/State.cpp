// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// For the license refer to https://github.com/dolphin-emu/dolphin/blob/master/license.txt
// This file is modified to fit RPCS3's purposes

#include "stdafx.h"
#include "Emu/State.h"
#include "Emu/Memory/vm.h"
#include "Emu/CPU/CPUThread.h"
#include "Emu/Cell/SPUThread.h"
#include "3rdparty/ChunkFile.h"
#include "Utilities/Thread.h"
#include "Emu/System.h"
#include "IdManager.h"

#include <sstream>

#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace State
{
	static std::vector<u8> g_current_buffer;
	static std::mutex g_cs_current_buffer;

	static std::string g_last_filename;

	enum
	{
		STATE_NONE = 0,
		STATE_SAVE = 1,
		STATE_LOAD = 2,
	};

	static std::string DoState(PointerWrap& p)
	{
		/*
		// Begin with video backend, so that it gets a chance to clear its caches and writeback modified
		// things to RAM
		g_video_backend->DoState(p);
		p.DoMarker("video_backend");

		if (SConfig::GetInstance().bWii)
			Wiimote::DoState(p);
		p.DoMarker("Wiimote");

		PowerPC::DoState(p);
		p.DoMarker("PowerPC");
		// CoreTiming needs to be restored before restoring Hardware because
		// the controller code might need to schedule an event if the controller has changed.
		CoreTiming::DoState(p);
		p.DoMarker("CoreTiming");
		HW::DoState(p);
		p.DoMarker("HW");
		Movie::DoState(p);
		p.DoMarker("Movie");
		Gecko::DoState(p);
		p.DoMarker("Gecko");
		*/

		vm::DoState(p);
		p.DoMarker("vm", 0x66);

		//get_current_cpu_thread()->DoState(p);
		//p.DoMarker("cpu_thread");

		// TODO: make DoState lambda?
		//idm::select<ppu_thread>([&p](u32, ppu_thread& ppu) { ppu.DoState(p); });
		//idm::select<ARMv7Thread>([&p](u32, ARMv7Thread& arm) { arm.DoState(p); });
		//idm::select<RawSPUThread>([&p](u32, RawSPUThread& raw_spu) { raw_spu.DoState(p); });
		idm::select<SPUThread>([&p](u32, SPUThread& spu) { spu.DoState(p); });

		return "";
	}

	void LoadFromBuffer(std::vector<u8>& buffer)
	{
		//Core::RunAsCPUThread([&] {
			u8* ptr = &buffer[0];
			PointerWrap p(&ptr, PointerWrap::MODE_READ);
			DoState(p);
		//});
	}

	void SaveToBuffer(std::vector<u8>& buffer)
	{
		//Core::RunAsCPUThread([&] {
			u8* ptr = nullptr;
			PointerWrap p(&ptr, PointerWrap::MODE_MEASURE);

			DoState(p);
			const size_t buffer_size = reinterpret_cast<size_t>(ptr);
			buffer.resize(buffer_size);

			ptr = &buffer[0];
			p.SetMode(PointerWrap::MODE_WRITE);
			DoState(p);
		//});
	}

	void VerifyBuffer(std::vector<u8>& buffer)
	{
		//Core::RunAsCPUThread([&] {
			u8* ptr = &buffer[0];
			PointerWrap p(&ptr, PointerWrap::MODE_VERIFY);
			DoState(p);
		//});
	}

	// return state number not in map
	static int GetEmptySlot(std::map<double, int> m)
	{
		for (int i = 1; i <= (int)NUM_STATES; i++)
		{
			bool found = false;
			for (auto& p : m)
			{
				if (p.second == i)
				{
					found = true;
					break;
				}
			}
			if (!found)
				return i;
		}
		return -1;
	}

	static std::string MakeStateFilename(int number);

	// read state timestamps
	//static std::map<double, int> GetSavedStates()
	//{
	//	StateHeader header;
	//	std::map<double, int> m;
	//	for (int i = 1; i <= (int)NUM_STATES; i++)
	//	{
	//		std::string filename = MakeStateFilename(i);
	//		if (fs::is_file(filename))
	//		{
	//			if (ReadHeader(filename, header))
	//			{
	//				double d = Common::Timer::GetDoubleTime() - header.time;
	//
	//				// increase time until unique value is obtained
	//				while (m.find(d) != m.end())
	//					d += .001;
	//
	//				m.emplace(d, i);
	//			}
	//		}
	//	}
	//	return m;
	//}

	struct CompressAndDumpState_args
	{
		std::vector<u8>* buffer_vector;
		std::mutex* buffer_mutex;
		std::string filename;
	};

	static void CompressAndDumpState(CompressAndDumpState_args save_args)
	{
		const u8* const buffer_data = &(*(save_args.buffer_vector))[0];
		const size_t buffer_size = (save_args.buffer_vector)->size();
		std::string& filename = save_args.filename;

		// For easy debugging
		// Common::SetCurrentThreadName("SaveState thread");

		// Moving to last overwritten save-state
		if (fs::is_file(filename))
		{
			if (fs::is_file("lastState.sav"))
				fs::remove_file(("lastState.sav"));

			if (!fs::rename(filename, "lastState.sav", true))
				LOG_ERROR(GENERAL, "Failed to move previous state to state undo backup");
		}

		fs::file f(filename, fs::rewrite);
		if (!f)
		{
			LOG_ERROR(GENERAL, "Could not save state");
			return;
		}

		// Setting up the header
		StateHeader header;
		const auto title_id = Emu.GetTitleID();
		title_id.copy(header.gameID, 9);
		header.size = 0;
		header.time = 0; // Common::Timer::GetDoubleTime();
		f.write(&header, sizeof(header));

		f.write(buffer_data, buffer_size);

		LOG_SUCCESS(GENERAL, "Saved State to %s", filename.c_str());
	}

	void SaveAs(const std::string& filename, bool wait)
	{
		//Core::RunAsCPUThread([&] {
			// Measure the size of the buffer.
			u8* ptr = nullptr;
			PointerWrap p(&ptr, PointerWrap::MODE_MEASURE);
			DoState(p);
			const size_t buffer_size = reinterpret_cast<size_t>(ptr);

			// Then actually do the write.
			{
				std::lock_guard<std::mutex> lk(g_cs_current_buffer);
				g_current_buffer.resize(buffer_size);
				ptr = &g_current_buffer[0];
				p.SetMode(PointerWrap::MODE_WRITE);
				DoState(p);
			}

			if (p.GetMode() == PointerWrap::MODE_WRITE)
			{
				LOG_NOTICE(GENERAL, "Saving State...");

				CompressAndDumpState_args save_args;
				save_args.buffer_vector = &g_current_buffer;
				save_args.buffer_mutex = &g_cs_current_buffer;
				save_args.filename = filename;

				Flush();
				CompressAndDumpState(save_args);

				g_last_filename = filename;
			}
			else
			{
				// someone aborted the save by changing the mode?
				LOG_NOTICE(GENERAL, "Unable to save: Internal DoState Error");
			}
		//});
	}

	bool ReadHeader(const std::string& filename, StateHeader& header)
	{
		Flush();
		fs::file f(filename, fs::read);
		if (!f)
		{
			LOG_NOTICE(GENERAL, "State not found");
			return false;
		}

		f.read(&header, sizeof(header));
		return true;
	}

	std::string GetInfoStringOfSlot(int slot, bool translate)
	{
		std::string filename = MakeStateFilename(slot);
		if (!fs::is_file(filename))
			return "Empty";

		State::StateHeader header;
		if (!ReadHeader(filename, header))
			return "Unknown";

		return "todo:time";//Common::Timer::GetDateTimeFormatted(header.time);
	}

	static void LoadFileStateData(const std::string& filename, std::vector<u8>& ret_data)
	{
		Flush();
		fs::file f(filename, fs::read);
		if (!f)
		{
			LOG_NOTICE(GENERAL, "State not found");
			return;
		}

		StateHeader header;
		f.read(&header, sizeof(header));

		if (strncmp(Emu.GetTitleID().c_str(), header.gameID, 9))
		{
			LOG_NOTICE(GENERAL, "State belongs to a different game (ID %.*s)", 9, header.gameID);
			return;
		}

		std::vector<u8> buffer;

		{
			const size_t size = (size_t)(f.size() - sizeof(StateHeader));
			buffer.resize(size);

			if (!f.read(&buffer[0], size))
			{
				LOG_FATAL(GENERAL, "wtf? reading bytes: %zu", size);
				return;
			}
		}

		// all good
		ret_data.swap(buffer);
	}

	void LoadAs(const std::string& filename)
	{
		//if (!Core::IsRunning())
		//{
		//	return;
		//}

		//Core::RunAsCPUThread([&] {
			bool loaded = false;
			bool loadedSuccessfully = false;
			std::string version_created_by;

			// brackets here are so buffer gets freed ASAP
			{
				std::vector<u8> buffer;
				LoadFileStateData(filename, buffer);

				if (!buffer.empty())
				{
					u8* ptr = &buffer[0];
					PointerWrap p(&ptr, PointerWrap::MODE_READ);
					version_created_by = DoState(p);
					loaded = true;
					loadedSuccessfully = (p.GetMode() == PointerWrap::MODE_READ);
				}
			}

			if (loaded)
			{
				if (loadedSuccessfully)
				{
					LOG_SUCCESS(GENERAL, "Loaded state from %s", filename.c_str());
				}
				else
				{
					LOG_ERROR(GENERAL, "Unable to load");
				}
			}
		//});
	}

	void VerifyAt(const std::string& filename)
	{
		//Core::RunAsCPUThread([&] {
			std::vector<u8> buffer;
			LoadFileStateData(filename, buffer);

			if (!buffer.empty())
			{
				u8* ptr = &buffer[0];
				PointerWrap p(&ptr, PointerWrap::MODE_VERIFY);
				DoState(p);

				if (p.GetMode() == PointerWrap::MODE_VERIFY)
					LOG_SUCCESS(GENERAL, "Verified state at %s", filename.c_str());
				else
					LOG_ERROR(GENERAL, "Unable to Verify : Can't verify state from other revisions !");
			}
		//});
	}

	void Init()
	{
	}

	void Shutdown()
	{
		Flush();
	}

	static std::string MakeStateFilename(int number)
	{
		return fmt::format("%s%s.s%02i", "",
			Emu.GetTitleID().c_str(), number);
	}

	void Save(int slot, bool wait)
	{
		Emu.Pause();
		SaveAs(MakeStateFilename(slot), wait);
		Emu.Resume();
	}

	void Load(int slot)
	{
		Emu.Pause();
		LoadAs(MakeStateFilename(slot));
		Emu.Resume();
	}

	void Verify(int slot)
	{
		VerifyAt(MakeStateFilename(slot));
	}

	//void LoadLastSaved(int i)
	//{
	//	std::map<double, int> savedStates = GetSavedStates();

	//	if (i > (int)savedStates.size())
	//		LOG_NOTICE(GENERAL, ("State doesn't exist", 2000);
	//	else
	//	{
	//		std::map<double, int>::iterator it = savedStates.begin();
	//		std::advance(it, i - 1);
	//		Load(it->second);
	//	}
	//}

	// must wait for state to be written because it must know if all slots are taken
	//void SaveFirstSaved()
	//{
	//	std::map<double, int> savedStates = GetSavedStates();

	//	// save to an empty slot
	//	if (savedStates.size() < NUM_STATES)
	//		Save(GetEmptySlot(savedStates), true);
	//	// overwrite the oldest state
	//	else
	//	{
	//		std::map<double, int>::iterator it = savedStates.begin();
	//		std::advance(it, savedStates.size() - 1);
	//		Save(it->second, true);
	//	}
	//}

	void Flush()
	{
	}

}  // namespace State
