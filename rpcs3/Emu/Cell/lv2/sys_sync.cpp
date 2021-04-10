#include "stdafx.h"
#include "sys_sync.h"

#include "Emu/IdManager.h"
#include "sys_lwmutex.h"

#include <cereal/archives/binary.hpp>
#include <cereal/types/vector.hpp>

#include <vector>
#include <utility>

LOG_CHANNEL(sys_sync);

// TODO: generalize / 'generic' idm serialize funcs (accepts type as template & maybe id type mask as arg ?)
//       ideally we'll just be able to declare ie. "serialize all <lv2_obj, lv2_lwmutex>" and it'll generate the code below
// TODO: will need to solve cereal needing default ctor issue. can just add default ctor to required classes (?)

// EXPERIMENTING
namespace lv2
{
	template <class Archive>
	void save(Archive& ar)
	{
		if (!lv2_obj::g_to_awake.empty())
			__debugbreak();
		if (!lv2_obj::g_pending.empty())
			__debugbreak();
		//if (!lv2_obj::g_waiting.empty())
		//	__debugbreak();

		std::vector<u32> ids;
		auto on_select_lv2_lwmutex = [&](u32 id, lv2_lwmutex& lwmutex) {
			ids.push_back(id);
		};
		idm::select<lv2_obj, lv2_lwmutex>(on_select_lv2_lwmutex);
		ar(ids);

		for (auto id : ids)
		{
			if (auto lwmutex = idm::get<lv2_obj, lv2_lwmutex>(id))
				lv2::save_lwmutex(ar, *lwmutex);
			else
				__debugbreak();
		}
	}

	// TODO: maybe don't return shared_ptr, use move / rvalue ref or whatever

	template <class Archive>
	void load(Archive& ar)
	{
		// TODO: idm remove_all (?)
		std::vector<u32> ids_to_remove;
		auto on_select_lv2_lwmutex = [&](u32 id, lv2_lwmutex& lwmutex) {
			ids_to_remove.push_back(id);
		};
		for (auto id : ids_to_remove)
			idm::remove<lv2_obj, lv2_lwmutex>(id);


		std::vector<u32> ids;
		ar(ids);
		for (auto id : ids)
		{
			if (!idm::import_existing_id<lv2_obj, lv2_lwmutex>(id, lv2::load_lwmutex(ar)))
				__debugbreak();
		}
	}

	template void save<>(cereal::BinaryOutputArchive& ar);
	template void load<>(cereal::BinaryInputArchive& ar);
} // namespace lv2
