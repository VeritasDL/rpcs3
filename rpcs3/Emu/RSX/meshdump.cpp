#include "stdafx.h"
#include "meshdump.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdint>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <Emu/RSX/stb_image_write.h>
#include <Emu/RSX/s3tc.h>
#include <Emu/RSX/umHalf.h>
#include <Emu/RSX/RSXThread.h>
#include <Emu/System.h>

#define MESHDUMP_DEBUG false
#define MESHDUMP_DEBUG_OLD false
#define MESHDUMP_POSED true
#define MESHDUMP_SLY_VERSION 3
#define MESHDUMP_NOCLIP true
#define MESHDUMP_BATCH_DUMPS true

//#pragma optimize("", off)

using namespace rsx;

// todo move
namespace neolib
{
	template <class Elem, class Traits>
	inline void hex_dump(const void* aData, std::size_t aLength, std::basic_ostream<Elem, Traits>& aStream, std::size_t aWidth = 16, ::std::string prepend_line = "")
	{
		const char* const start = static_cast<const char*>(aData);
		const char* const end   = start + aLength;
		const char* line        = start;
		while (line != end)
		{
			aStream << prepend_line;
			aStream.width(4);
			aStream.fill('0');
			aStream << std::hex << line - start << " ";
			std::size_t lineLength = std::min(aWidth, static_cast<std::size_t>(end - line));
			for (std::size_t pass = 1; pass <= 3; ++pass)
			{
				if (pass == 3)
				{
					const auto word_count = aWidth / 4;
					for (const char* next = line; next < line + word_count * 4; next += 4)
					{
						const auto v = _byteswap_ulong(*(const u32*)(next));
						aStream << *(const float*)&v;
						aStream << " ";
					}
					break;
				}

				for (const char* next = line; next != end && next != line + aWidth; ++next)
				{
					char ch = *next;
					switch (pass)
					{
					case 1:
						aStream << (ch < 32 ? '.' : ch);
						break;
					case 2:
						if (next != line)
							aStream << " ";
						aStream.width(2);
						aStream.fill('0');
						aStream << std::hex << std::uppercase << static_cast<int>(static_cast<unsigned char>(ch));
						break;
					}
				}
				if (pass == 1 && lineLength != aWidth)
					aStream << std::string(aWidth - lineLength, ' ');
				aStream << " ";
			}
			aStream << std::endl;
			line = line + lineLength;
		}
	}
} // namespace neolib

// key is tex_raw_data_ptr_t (can't actually use it)
std::map<u64, texture_info_t> g_dump_texture_info;
mesh_dumper g_mesh_dumper{};
std::mutex g_mesh_dumper_mtx{};
u32 g_clears_this_frame{};

void mesh_dumper::push_block(const mesh_draw_dump_block& block)
{
	auto& mesh_draw_dump = dumps.back();
	mesh_draw_dump.blocks.push_back(block);
}

void mesh_dumper::push_dump(const mesh_draw_dump& dump)
{
	dumps.push_back(dump);
}

mesh_draw_dump& mesh_dumper::get_dump()
{
	return dumps.back();
}

void mesh_dumper::dump()
{
	Emu.Pause();

	static int session_id = time(NULL) & 0xFFFFFF;
	static u32 dump_index{};

	const std::string dump_dir = fmt::format("meshdump_%X", session_id);
	std::filesystem::create_directory(dump_dir);

	//
	// DUMP MATERIALS
	//

	// TODO(?): hash-based texture storage/indexing

	const std::string mtl_file_name = fmt::format("%s/meshdump_%X_%d.mtl", dump_dir.c_str(), session_id, dump_index);

	std::ofstream file_mtl(mtl_file_name);
	std::string mtl_str;

	std::vector<u8> work_buf;
	std::vector<u8> final_data;
	for (auto [raw_data_ptr, info_] : g_dump_texture_info)
	{
		if (!info_.is_used)
			continue;

		const std::string tex_dir_name     = fmt::format("%s/textures_%d", dump_dir.c_str(), dump_index);
		const std::string tex_dir_name_rel = fmt::format("textures_%d", dump_index);
		std::filesystem::create_directory(tex_dir_name);

		const std::string tex_file_name     = fmt::format("%s/0x%X.png", tex_dir_name, (u64)raw_data_ptr);
		const std::string tex_file_name_rel = fmt::format("%s/0x%X.png", tex_dir_name_rel, (u64)raw_data_ptr);

		const auto src = (const unsigned char*)((tex_raw_data_ptr_t)raw_data_ptr)->data();

		if (src == 0 || (u64)src == 0xf || ((u64)src & 0x8000000000000000)) // hacky af
		{
			mtl_str += fmt::format("# warning: skipped texture, src is 0\n");
			continue;
		}

		final_data.resize(info_.width * info_.height * 4);

		bool is_fully_opaque = true;

		if (info_.format == CELL_GCM_TEXTURE_COMPRESSED_DXT45)
		{
			BlockDecompressImageDXT5(info_.width, info_.height, src, (unsigned long*)final_data.data());
			for (auto i = 0; i < final_data.size() / 4; ++i)
			{
				u32* ptr    = &((u32*)final_data.data())[i];
				const u32 v = *ptr;
				if ((v & 0x000000FF) == 0x00000080)
				{
					*ptr = (((v & 0xFF000000) >> 24) |
							((v & 0x00FF0000) >> 8) |
							((v & 0x0000FF00) << 8) |
							0xFF000000);
				}
				else
				{
					*ptr            = (((v & 0xFF000000) >> 24) |
                            ((v & 0x00FF0000) >> 8) |
                            ((v & 0x0000FF00) << 8) |
                            (((v & 0x000000FF) * 2) << 24));
					is_fully_opaque = false;
				}
			}
		}
		else if (info_.format == CELL_GCM_TEXTURE_COMPRESSED_DXT1) // Sly 4
		{
			BlockDecompressImageDXT1(info_.width, info_.height, src, (unsigned long*)final_data.data());
			for (auto i = 0; i < final_data.size() / 4; ++i)
			{
				u32* ptr    = &((u32*)final_data.data())[i];
				const u32 v = *ptr;
				if ((v & 0x000000FF) != 0x000000FF)
				{
					is_fully_opaque = false;
				}
				*ptr = (((v & 0xFF000000) >> 24) |
						((v & 0x00FF0000) >> 8) |
						((v & 0x0000FF00) << 8) |
						(((v & 0x000000FF) * 2) << 24));
			}
		}
		else if ((info_.format & CELL_GCM_TEXTURE_A8R8G8B8) == CELL_GCM_TEXTURE_A8R8G8B8)
		{
			memcpy(final_data.data(), src, info_.width * info_.height * 4);
			for (auto i = 0; i < final_data.size() / 4; ++i)
			{
				u32* ptr    = &((u32*)final_data.data())[i];
				*ptr        = _byteswap_ulong(*ptr);
				const u32 v = *ptr;
				//is_fully_opaque &= ((v & 0xFF000000) == 0x80000000);
				if ((v & 0x000000FF) == 0x00000080)
				{
					*ptr = (((v & 0xFF000000) >> 8) |
							((v & 0x00FF0000) >> 8) |
							((v & 0x0000FF00) >> 8) |
							0xFF000000);
				}
				else
				{
					*ptr            = (((v & 0xFF000000) >> 8) |
                            ((v & 0x00FF0000) >> 8) |
                            ((v & 0x0000FF00) >> 8) |
                            (((v & 0x000000FF) * 2) << 24));
					is_fully_opaque = false;
				}
			}
		}
		//else
		//{
		//	mtl_str += fmt::format("# warning: skipped texture, format is 0x%X\n", info_.format);
		//}

		// argb -> rgba
		// also a sly specific thing, it's PS2 port, and GS's RGBA reg alpha is weird like that

		mtl_str += fmt::format("newmtl 0x%X\n", (u64)raw_data_ptr);
		mtl_str += fmt::format("map_Kd %s\n", tex_file_name_rel.c_str());
		mtl_str += fmt::format("Ns 10\n"); // TODO: check
		if (!is_fully_opaque)
			mtl_str += fmt::format("map_D %s\n", tex_file_name_rel.c_str());

		work_buf.resize(final_data.size());
		// copy flipped on Y
		const auto stride = info_.width * 4;
		for (auto i = 0; i < info_.height; i++)
		{
			memcpy(work_buf.data() + i * stride, final_data.data() + (final_data.size() - ((i + 1) * stride)), stride);
		}

		if (!stbi_write_png(tex_file_name.c_str(), info_.width, info_.height, 4, work_buf.data(), info_.width * 4))
			__debugbreak();
	}

#if MESHDUMP_NOCLIP
	work_buf.resize(4);
	work_buf[0] = 0x00;
	work_buf[1] = 0x00;
	work_buf[2] = 0x00;
	work_buf[3] = 0x00;
	for (auto& d : g_mesh_dumper.dumps)
	{
		const std::string tex_dir_name  = fmt::format("%s/textures_%d", dump_dir.c_str(), dump_index);
		const std::string tex_file_name = fmt::format("%s/0x%X.png", tex_dir_name, (u64)d.texture_raw_data_ptr);

		if (!std::filesystem::exists(tex_file_name))
			if (!stbi_write_png(tex_file_name.c_str(), 1, 1, 4, work_buf.data(), 1 * 4))
				__debugbreak();
	}
#endif

	file_mtl.write(mtl_str.c_str(), mtl_str.size());

	
	//
	// DUMP MESHES
	//

	std::ofstream file_obj(fmt::format("%s/meshdump_%X_%d.obj", dump_dir.c_str(), session_id, dump_index));
	std::string obj_str;

	obj_str += fmt::format("mtllib meshdump_%X_%d.mtl\n", session_id, dump_index);

	u32 vertex_index_base{1};
#if MESHDUMP_NOCLIP
	vertex_index_base = 0;
#endif
	u32 vertex_index_base_normal_offset{};

	//g_mesh_dumper.dumps[4].vertex_constants_buffer

	mat4 inv_view_mat = linalg::identity;
#if MESHDUMP_SLY_VERSION == 3
	auto& v      = inv_view_mat;
	auto m       = (be_t<float>*)vm::base(0x007DA920);
	int i        = 0;
	v[0][0] = m[i++]; v[0][1] = m[i++]; v[0][2] = m[i++]; v[0][3] = m[i++];
	v[1][0] = m[i++]; v[1][1] = m[i++]; v[1][2] = m[i++]; v[1][3] = m[i++];
	v[2][0] = m[i++]; v[2][1] = m[i++]; v[2][2] = m[i++]; v[2][3] = m[i++];
	v[3][0] = m[i++]; v[3][1] = m[i++]; v[3][2] = m[i++]; v[3][3] = m[i++];
	inv_view_mat = linalg::inverse(inv_view_mat);
#endif
	std::map<u32, u32> hmm;
	for (auto dump_idx = 0; dump_idx < g_mesh_dumper.dumps.size(); ++dump_idx)
	{
		auto& d = g_mesh_dumper.dumps[dump_idx];
		hmm[(u32)d.texture_raw_data_ptr]++;
	}

#if MESHDUMP_BATCH_DUMPS
	std::sort(g_mesh_dumper.dumps.begin(), g_mesh_dumper.dumps.end(), [](const mesh_draw_dump& d0, const mesh_draw_dump& d1) {
		return (u64)d0.texture_raw_data_ptr < (u64)d1.texture_raw_data_ptr;
	});
#endif
	bool new_dump = true;

	for (auto dump_idx = 0; dump_idx < g_mesh_dumper.dumps.size(); ++dump_idx)
	{
		auto& d = g_mesh_dumper.dumps[dump_idx];

#if MESHDUMP_BATCH_DUMPS
		if (dump_idx > 0)
			new_dump = ((u64)d.texture_raw_data_ptr != (u64)g_mesh_dumper.dumps[dump_idx - 1].texture_raw_data_ptr);
#endif

		obj_str += fmt::format("%s %d_%X_vshd:%08X_fshd:%08X_tex:%X_clr:%d_blk:%d\n",
			new_dump ? "o" : "g", dump_idx, session_id, d.vert_shader_hash, d.frag_shader_hash, (u32)d.texture_raw_data_ptr, d.clear_count, d.blocks.size());

#if MESHDUMP_DEBUG
		if (auto it = g_dump_texture_info.find((u64)d.texture_raw_data_ptr); it != g_dump_texture_info.end())
		{
			obj_str += fmt::format("# Texture %dx%d fmt 0x%X\n", it->second.width, it->second.height, it->second.format);
		}
#endif
		//if (g_dump_texture_info.contains((u64)d.texture_raw_data_ptr))
		//{
		//	const auto src = (const unsigned char*)((tex_raw_data_ptr_t)d.texture_raw_data_ptr)->data();

		//	if (src == 0 || (u64)src == 0xf || ((u64)src & 0x8000000000000000)) { // hacky af
		//		obj_str += fmt::format("usemtl 0x0\n");
		//	}
		//	else
		//	{
		//		obj_str += fmt::format("usemtl 0x%X\n", d.texture_raw_data_ptr);
		//	}
		//}
		//else
		//{
		//	obj_str += fmt::format("usemtl 0x0\n");
		//}
		if (new_dump)
			obj_str += fmt::format("usemtl 0x%X\n", d.texture_raw_data_ptr);

		//auto dst_ptr = d.vertex_data.data();
		//std::stringstream hex_ss;
		//neolib::hex_dump((const void*)dst_ptr, d.vertex_data.size(), hex_ss, 36);
		//auto hex_str = hex_ss.str();
		////hex_str.pop_back(); // remove newline
		//obj_str += hex_str;

#if MESHDUMP_DEBUG
		for (int i = 0; i < d.blocks.size(); ++i)
		{
			std::string hex_str;
			const auto& block                    = d.blocks[i];
			auto interleaved_attribute_array_str = [](rsx::simple_array<interleaved_attribute_t> locations) -> std::string {
				std::string locs_str = "{ ";
				for (const auto& l : locations)
				{
					if (l.modulo == 0 && l.frequency == 0)
						locs_str += fmt::format("%d,", l.index);
					else
						locs_str += fmt::format("%d(%d,%d),", l.index, l.modulo, l.frequency);
				}
				locs_str.erase(locs_str.size() - 1, 1);
				locs_str += " }";
				return locs_str;
			};
			hex_str += fmt::format("# Block %d, %s, locs: %s, count: %d, size: %d\n",
				i, block.interleaved_range_info.to_str().c_str(),
				interleaved_attribute_array_str(block.interleaved_range_info.locations),
				block.vertex_data.size() / block.interleaved_range_info.attribute_stride, block.vertex_data.size());

#if MESHDUMP_SLY_VERSION != 4
			//if (i == 0 && (block.interleaved_range_info.attribute_stride == 36 || block.interleaved_range_info.attribute_stride == 28))
			//{
			//hex_str += " # skipping block data log\n";
			//}
			//else
#endif
			{
				auto dst_ptr = block.vertex_data.data();
				std::stringstream hex_ss;
				auto hex_width = block.interleaved_range_info.attribute_stride;
				if (block.interleaved_range_info.attribute_stride <= 4 &&
					((d.blocks[i].vertex_data.size() % 64) == 0))
					hex_width = 64;
				neolib::hex_dump((const void*)dst_ptr, d.blocks[i].vertex_data.size(), hex_ss, hex_width, " # ");
				hex_str += hex_ss.str();
				//hex_str.pop_back(); // remove newline
			}
			obj_str += hex_str;
		}

		if (!d.volatile_data.empty())
		{
			obj_str += fmt::format("# Volatile Data size 0x%X\n", d.volatile_data.size());
			auto dst_ptr = d.volatile_data.data();
			std::stringstream hex_ss;
			neolib::hex_dump((const void*)dst_ptr, d.volatile_data.size(), hex_ss, 32, " # ");
			auto hex_str = hex_ss.str();
			//hex_str.pop_back(); // remove newline
			obj_str += hex_str;
		}
#endif

		// TODO: move to structs

		//auto dot = [](const float4& v0, const float4& v1) -> float {
		//	return v0.x * v1.x + v0.y * v1.y + v0.z * v1.z + v0.w * v1.w;
		//};

		auto mul = [&](const float4& v0, const mat4& v1) -> float4 {
			return {
				dot(v0, v1[0]),
				dot(v0, v1[1]),
				dot(v0, v1[2]),
				dot(v0, v1[3])};
		};

		auto print_vec4 = [](float4 v) {
			return fmt::format("# { %5.2f, %5.2f, %5.2f, %5.2f }", v.x, v.y, v.z, v.w);
		};

		auto print_mat43 = [](float4 _0, float4 _1, float4 _2) {
			return fmt::format("# ((( %5.2f, %5.2f, %5.2f, %5.2f ),\n#   ( %5.2f, %5.2f, %5.2f, %5.2f ),\n#   ( %5.2f, %5.2f, %5.2f, %5.2f ),\n#   ( %5.2f, %5.2f, %5.2f, %5.2f )))",
				_0.x, _1.x, _2.x, 0.0,
				_0.y, _1.y, _2.y, 0.0,
				_0.z, _1.z, _2.z, 0.0,
				_0.w, _1.w, _2.w, 1.0);
		};

		auto print_mat4 = [](float4 _0, float4 _1, float4 _2, float4 _3) {
			return fmt::format("# ((( %5.2f, %5.2f, %5.2f, %5.2f ),\n#   ( %5.2f, %5.2f, %5.2f, %5.2f ),\n#   ( %5.2f, %5.2f, %5.2f, %5.2f ),\n#   ( %5.2f, %5.2f, %5.2f, %5.2f )))",
				_0.x, _1.x, _2.x, _3.x,
				_0.y, _1.y, _2.y, _3.y,
				_0.z, _1.z, _2.z, _3.z,
				_0.w, _1.w, _2.w, _3.w);
		};

		auto print_mat4_ = [&](mat4 m) {
			return print_mat4(m[0], m[1], m[2], m[3]);
		};

		const auto block_count = d.blocks.size();
		const auto index_count = d.indices.size();

		const auto& vcb = d.vertex_constants_buffer;

#if MESHDUMP_SLY_VERSION != 4
		const bool use_no_xform = (block_count == 1);
		const bool has_bones    = (block_count == 4);

		mat4 xform_mat       = linalg::identity;
		mat4 xform_mat_first = linalg::identity;
		mat4 bone_mats[4];

		if (vcb.size() >= 17)
		{
			xform_mat_first = {vcb[0], vcb[1], vcb[2], vcb[3]};
			xform_mat       = {vcb[13], vcb[14], vcb[15], vcb[16]};

			if (has_bones && vcb.size() >= 44)
			{
				bone_mats[0] = {vcb[32], vcb[33], vcb[34], {0, 0, 0, 1}};
				bone_mats[1] = {vcb[35], vcb[36], vcb[37], {0, 0, 0, 1}};
				bone_mats[2] = {vcb[38], vcb[39], vcb[40], {0, 0, 0, 1}};
				bone_mats[3] = {vcb[41], vcb[42], vcb[43], {0, 0, 0, 1}};
			}
		}

		auto transform_pos = [&](u32 idx, const vec3be& pos, const mesh_draw_dump_block* weights_block) -> vec3 {
			vec4 out;
			vec4 a = {pos.x, pos.y, pos.z, 1};
			if (has_bones)
			{
#if MESHDUMP_POSED
				const vec4be* weight_array = (vec4be*)weights_block->vertex_data.data();
				if (idx * 16 < weights_block->vertex_data.size())
				{
					const vec4be weights_be = weight_array[idx];
					const vec4 weights      = {weights_be.x, weights_be.y, weights_be.z, weights_be.w};

					const vec4 r0 = weights.x * bone_mats[0][0] + weights.y * bone_mats[1][0] + weights.z * bone_mats[2][0] + weights.w * bone_mats[3][0];
					const vec4 r1 = weights.x * bone_mats[0][1] + weights.y * bone_mats[1][1] + weights.z * bone_mats[2][1] + weights.w * bone_mats[3][1];
					const vec4 r2 = weights.x * bone_mats[0][2] + weights.y * bone_mats[1][2] + weights.z * bone_mats[2][2] + weights.w * bone_mats[3][2];

					out = {
						dot(a, r0),
						dot(a, r1),
						dot(a, r2),
						1};

#if MESHDUMP_DEBUG
					obj_str += fmt::format("# weights: %f %f %f %f, rvecs: %s\n%s\n%s\n# out: %s\n",
						weights[0], weights[1], weights[2], weights[3], print_vec4(r0), print_vec4(r1), print_vec4(r2), print_vec4(out));
#endif

					//auto isbig = [](double x)
					//{
					//	return fabs(x) > FLT_MAX;
					//};
					//if (isnan(out.x) || isnan(out.y) || isnan(out.z) || isnan(out.w) ||
					//	isinf(out.x) || isinf(out.y) || isinf(out.z) || isinf(out.w) ||
					//    isbig(out.x) || isbig(out.y) || isbig(out.z) || isbig(out.w))
					const double sum     = weights.x + weights.y + weights.z + weights.w;
					const double epsilon = 0.01;
					if ((sum > 1. + epsilon) || (sum < 1. - epsilon) || isnan(sum))
					{
						// TODO: don't emit these
						obj_str += "# bad weights, defaulting to first bone\n";
						out = {
							dot(a, bone_mats[0][0]),
							dot(a, bone_mats[0][1]),
							dot(a, bone_mats[0][2]),
							1};
					}
				}
				else
				{
					obj_str += fmt::format("# warning: weights out of bounds\n");
					//__debugbreak();
				}
				out = mul(out, xform_mat);
#else
				out = mul(a, xform_mat);
#endif
			}
			else if (use_no_xform)
			//else
			{
				//out = mul(a, xform_mat_first);
				//__debugbreak();
				obj_str += "# using no xform\n";
				out = a;
			}
			else
			{
				out = mul(a, xform_mat);
			}
			//return {out.x * 0.01f, out.y * 0.01f, out.z * 0.01f};

#if MESHDUMP_SLY_VERSION == 3
			out = mul(out, inv_view_mat);
			// xform_mat = linalg::mul(xform_mat, inv_view_mat);
#endif

			return {out.x, out.y, out.z};
		};
#endif

		size_t vertex_count = 0;

		enum class vertex_format_t
		{
			none,
			_28,
			_30,
			_36,
			sly4_14,
			sly4_18,
			sly4_24,
		} vertex_format{};

#if MESHDUMP_SLY_VERSION != 4
		// TODO: vertex color support

		enum draw_type_t
		{
			sly3_normal,
			sly3_skydome,

			invalid = -1,
		} draw_type = invalid;

		auto emit_vc_data = [&](int draw_type) {
			// todo: emit binary
			obj_str += fmt::format("vc %d %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f\n",
				draw_type,
				vcb[17].x, vcb[17].y, vcb[17].z, vcb[17].w,
				vcb[18].x, vcb[18].y, vcb[18].z, vcb[18].w,
				vcb[19].x, vcb[19].y, vcb[19].z, vcb[19].w,
				vcb[29].x, vcb[29].y, vcb[29].z, vcb[29].w,
				vcb[0].x, vcb[0].y, vcb[0].z, vcb[0].w,
				vcb[1].x, vcb[1].y, vcb[1].z, vcb[1].w,
				vcb[2].x, vcb[2].y, vcb[2].z, vcb[2].w,
				vcb[3].x, vcb[3].y, vcb[3].z, vcb[3].w);
		};

		if (block_count > 0)
		{
			const auto& block0         = d.blocks[0];
			const auto* block1_weights = has_bones ? &d.blocks[1] : nullptr;

			if (block0.interleaved_range_info.interleaved)
			{

				if (d.vert_shader_hash == 0x47DA8B28 && d.frag_shader_hash == 0x0BBE0FF4)
					draw_type = sly3_normal;
				else if (d.vert_shader_hash == 0x73783E15 && d.frag_shader_hash == 0xFFA26447)
					draw_type = sly3_skydome;

#if MESHDUMP_NOCLIP
				//if (block0.interleaved_range_info.attribute_stride == 36 && block_count == 3) {
				if (draw_type == sly3_normal || draw_type == sly3_skydome)
				{
					emit_vc_data((int)draw_type);

					struct mesh_draw_vertex_36
					{
						vec3be pos;
						vec3be normal;
						vec2be uv;
						u32 unk0;
					};
					static_assert(sizeof(mesh_draw_vertex_36) == 36);

					const mesh_draw_vertex_36* vertex_data = (mesh_draw_vertex_36*)block0.vertex_data.data();
					vertex_count                           = block0.vertex_data.size() / sizeof(mesh_draw_vertex_36);

					for (auto i = 0; i < vertex_count; ++i)
					{
						const auto& v  = vertex_data[i];
						const vec3 pos = transform_pos(i, v.pos, block1_weights);
						const u32 diff       = ((u32*)d.blocks[1].vertex_data.data())[i];

						const bool has_spec = (draw_type == sly3_normal);

						if (has_spec)
						{
							const u32 spec = ((u32*)d.blocks[2].vertex_data.data())[i];

							obj_str += fmt::format("v36s %f %f %f %f %f %f %f %f %X %X\n",
								pos.x, pos.y, pos.z, (float)v.normal.x, (float)v.normal.y, (float)v.normal.z,
								(float)v.uv.u, (float)v.uv.v,
								diff, spec);
						}
						else
						{
							obj_str += fmt::format("v36 %f %f %f %f %f %f %f %f %X\n",
								pos.x, pos.y, pos.z, (float)v.normal.x, (float)v.normal.y, (float)v.normal.z,
								(float)v.uv.u, (float)v.uv.v,
								diff);
						}
					}

					vertex_format = vertex_format_t::_36;
				}

#else
				if (block0.interleaved_range_info.attribute_stride == 36)
				{
					static bool b1{true};
					if (b1)
					{
						struct mesh_draw_vertex_36
						{
							vec3be pos;
							vec3be normal;
							vec2be uv;
							u32 unk0;
						};
						static_assert(sizeof(mesh_draw_vertex_36) == 36);

						const mesh_draw_vertex_36* vertex_data = (mesh_draw_vertex_36*)block0.vertex_data.data();
						vertex_count                           = block0.vertex_data.size() / sizeof(mesh_draw_vertex_36);

						for (auto i = 0; i < vertex_count; ++i)
						{
							const auto& v  = vertex_data[i];
							const vec3 pos = transform_pos(i, v.pos, block1_weights);
							// const auto posu = *(uvec3*)(&v.pos);
							obj_str += fmt::format("v %f %f %f\n", pos.x, pos.y, pos.z);
							obj_str += fmt::format("vn %f %f %f\n", (float)v.normal.x, (float)v.normal.y, (float)v.normal.z);
							obj_str += fmt::format("vt %f %f\n", (float)v.uv.u, (float)v.uv.v);
						}
						vertex_format = vertex_format_t::_36;
					}
				}
				else if (block0.interleaved_range_info.attribute_stride == 28)
				{
					static bool b2{true};
					if (b2)
					{
						struct mesh_draw_vertex_28
						{
							vec3be pos;
							u32 vertex_color_maybe;
							u32 unk0;
							vec2be uv;
						};
						static_assert(sizeof(mesh_draw_vertex_28) == 28);

						const mesh_draw_vertex_28* vertex_data = (mesh_draw_vertex_28*)block0.vertex_data.data();
						vertex_count                           = block0.vertex_data.size() / sizeof(mesh_draw_vertex_28);

						for (auto i = 0; i < vertex_count; ++i)
						{
							const auto& v  = vertex_data[i];
							const vec3 pos = transform_pos(i, v.pos, block1_weights);
							// const auto posu = *(uvec3*)(&v.pos);
							obj_str += fmt::format("v %f %f %f\n", pos.x, pos.y, pos.z);
							obj_str += fmt::format("vt %f %f\n", (float)v.uv.u, (float)v.uv.v);
						}

#if !MESHDUMP_NOCLIP
						vertex_index_base_normal_offset += vertex_count;
#endif
						vertex_format = vertex_format_t::_28;
					}
				}
#endif
			}
		}

#if 0
				for (auto& v : d.vertices)
				{
					obj_str += fmt::format("v %f %f %f\n", v.pos.x, v.pos.y, v.pos.z);
					obj_str += fmt::format("vt %f %f\n", v.uv.u, v.uv.v);
				}
#endif

#if MESHDUMP_DEBUG
		obj_str += fmt::format("# transform_branch_bits: 0x%08X\n", rsx::method_registers.transform_branch_bits());

		if (d.vertex_constants_buffer.size() >= 4)
		{
			obj_str += fmt::format("# xform matrix at 0:\n%s\n",
				print_mat4(d.vertex_constants_buffer[0], d.vertex_constants_buffer[1], d.vertex_constants_buffer[2], d.vertex_constants_buffer[3]));
		}
		if (d.vertex_constants_buffer.size() >= 17)
		{
			obj_str += fmt::format("# xform matrix at 13:\n%s\n",
				print_mat4(d.vertex_constants_buffer[13], d.vertex_constants_buffer[14], d.vertex_constants_buffer[15], d.vertex_constants_buffer[16]));
		}
		if (d.vertex_constants_buffer.size() >= 44)
		{
			obj_str += fmt::format("# xform matrices at 32:\n%s\n%s\n%s\n%s\n",
				print_mat43(d.vertex_constants_buffer[32], d.vertex_constants_buffer[33], d.vertex_constants_buffer[34]),
				print_mat43(d.vertex_constants_buffer[35], d.vertex_constants_buffer[36], d.vertex_constants_buffer[37]),
				print_mat43(d.vertex_constants_buffer[38], d.vertex_constants_buffer[39], d.vertex_constants_buffer[40]),
				print_mat43(d.vertex_constants_buffer[41], d.vertex_constants_buffer[42], d.vertex_constants_buffer[43]));
		}

		// and vc[18].xy is tex anim

#endif
#endif

#if MESHDUMP_DEBUG
		obj_str += "# VertexConstantsBuffer:\n";
		for (auto i = 0; i < 60; i++)
			obj_str += fmt::format(" # %02d: %s\n", i, print_vec4(d.vertex_constants_buffer[i]));
#endif

#if 1
#if MESHDUMP_SLY_VERSION == 4

#if MESHDUMP_DEBUG
		obj_str += fmt::format("# transform_branch_bits: 0x%08X\n", rsx::method_registers.transform_branch_bits());

		if (d.vertex_constants_buffer.size() >= 4)
			obj_str += fmt::format("# xform matrix at 0:\n%s\n",
				print_mat4(d.vertex_constants_buffer[0], d.vertex_constants_buffer[1], d.vertex_constants_buffer[2], d.vertex_constants_buffer[3]));
#endif

		if (block_count > 0)
		{
			const auto& block0 = d.blocks[0];

			if (block0.interleaved_range_info.interleaved)
			{

				mat4 xform_mat = linalg::identity;

				if (vcb.size() >= 4)
				{
					xform_mat = {vcb[0], vcb[1], vcb[2], vcb[3]};
				}

				auto transform_pos = [&](u32 idx, const auto& pos /*, const mesh_draw_dump_block* weights_block*/) -> vec3 {
					vec4 out;
					vec4 a = {pos.x, pos.y, pos.z, 1};

					static volatile bool s_posed = true;

					if (s_posed)
						out = mul(a, xform_mat);
					else
						out = a;

					return {-out.x, out.y, out.z};
				};

				if (block0.interleaved_range_info.attribute_stride == 24 && block_count == 2)
				{
					const auto& block1 = d.blocks[1];
					struct mesh_draw_vertex_24
					{
						vec3be pos;
						u8 unk[12];
					};
					static_assert(sizeof(mesh_draw_vertex_24) == 24);

					struct mesh_draw_vertex_24_block1
					{
						u32 unk_color;
						u16 texcoord_u;
						u16 texcoord_v;
					};
					static_assert(sizeof(mesh_draw_vertex_24_block1) == 8);

					const mesh_draw_vertex_24* vertex_data_0        = (mesh_draw_vertex_24*)block0.vertex_data.data();
					const mesh_draw_vertex_24_block1* vertex_data_1 = (mesh_draw_vertex_24_block1*)block1.vertex_data.data();
					vertex_count                                    = block0.vertex_data.size() / sizeof(mesh_draw_vertex_24);

					for (auto i = 0; i < vertex_count; ++i)
					{
						const auto& v        = vertex_data_0[i];
						const auto& v_block1 = vertex_data_1[i];

						vec3 pos = transform_pos(i, v.pos);
						//obj_str += fmt::format("v %f %f %f\n", (float)v.pos.x * .01, (float)v.pos.z * .01, (float)v.pos.y * .01);
						obj_str += fmt::format("v %f %f %f\n", pos.x, pos.y, pos.z);
						//obj_str += fmt::format("vn %f %f %f\n", (float)v.normal.x, (float)v.normal.y, (float)v.normal.z);
						half texcoord_u;
						texcoord_u.bits = _byteswap_ushort(v_block1.texcoord_u);
						half texcoord_v;
						texcoord_v.bits = _byteswap_ushort(v_block1.texcoord_v);
						obj_str += fmt::format("vt %f %f\n", (float)texcoord_u, (float)texcoord_v);
					}
					vertex_format = vertex_format_t::sly4_24;
				}
				else if (block0.interleaved_range_info.attribute_stride == 14 && block_count == 1)
				{
					struct mesh_draw_vertex_14
					{
						u16 pos_x;
						u16 pos_y;
						u16 pos_z;
						u8 color[4]; // weird format
						u16 texcoord_u;
						u16 texcoord_v;
					};
					static_assert(sizeof(mesh_draw_vertex_14) == 14);

					const mesh_draw_vertex_14* vertex_data_0 = (mesh_draw_vertex_14*)block0.vertex_data.data();
					vertex_count                             = block0.vertex_data.size() / sizeof(mesh_draw_vertex_14);

					for (auto i = 0; i < vertex_count; ++i)
					{
						const auto& v = vertex_data_0[i];

						half pos_x;
						pos_x.bits = _byteswap_ushort(v.pos_x);
						half pos_y;
						pos_y.bits = _byteswap_ushort(v.pos_y);
						half pos_z;
						pos_z.bits = _byteswap_ushort(v.pos_z);
						vec3 pos   = vec3{(float)pos_x, (float)pos_y, (float)pos_z};
						pos        = transform_pos(i, pos);
						//obj_str += fmt::format("v %f %f %f\n", (float)v.pos.x * .01, (float)v.pos.z * .01, (float)v.pos.y * .01);
						obj_str += fmt::format("v %f %f %f\n", (float)pos.x, (float)pos.y, (float)pos.z);
						//obj_str += fmt::format("vn %f %f %f\n", (float)v.normal.x, (float)v.normal.y, (float)v.normal.z);
						half texcoord_u;
						texcoord_u.bits = _byteswap_ushort(v.texcoord_u);
						half texcoord_v;
						texcoord_v.bits = _byteswap_ushort(v.texcoord_v);
						obj_str += fmt::format("vt %f %f\n", (float)texcoord_u, (float)texcoord_v);
					}
					vertex_format = vertex_format_t::sly4_14;
				}
				else if (block0.interleaved_range_info.attribute_stride == 18 && block_count == 1)
				{
					struct mesh_draw_vertex_18
					{
						u16 pos_x;
						u16 pos_y;
						u16 pos_z;
						u8 unk0[4]; // weird format
						u16 texcoord_u;
						u16 texcoord_v;
						u8 unk1[4]; // weird format
					};
					static_assert(sizeof(mesh_draw_vertex_18) == 18);

					const mesh_draw_vertex_18* vertex_data_0 = (mesh_draw_vertex_18*)block0.vertex_data.data();
					vertex_count                             = block0.vertex_data.size() / sizeof(mesh_draw_vertex_18);

					for (auto i = 0; i < vertex_count; ++i)
					{
						const auto& v = vertex_data_0[i];

						half pos_x;
						pos_x.bits = _byteswap_ushort(v.pos_x);
						half pos_y;
						pos_y.bits = _byteswap_ushort(v.pos_y);
						half pos_z;
						pos_z.bits = _byteswap_ushort(v.pos_z);
						vec3 pos   = vec3{(float)pos_x, (float)pos_y, (float)pos_z};
						pos        = transform_pos(i, pos);
						//obj_str += fmt::format("v %f %f %f\n", (float)v.pos.x * .01, (float)v.pos.z * .01, (float)v.pos.y * .01);
						obj_str += fmt::format("v %f %f %f\n", (float)pos.x, (float)pos.y, (float)pos.z);
						//obj_str += fmt::format("vn %f %f %f\n", (float)v.normal.x, (float)v.normal.y, (float)v.normal.z);
						half texcoord_u;
						texcoord_u.bits = _byteswap_ushort(v.texcoord_u);
						half texcoord_v;
						texcoord_v.bits = _byteswap_ushort(v.texcoord_v);
						obj_str += fmt::format("vt %f %f\n", (float)texcoord_u, (float)texcoord_v);
					}
					vertex_format = vertex_format_t::sly4_18;
				}
			}
		}
#endif
#endif

		if (vertex_count > 0 && index_count > 0)
		{
#if MESHDUMP_NOCLIP && MESHDUMP_BATCH_DUMPS
			if (new_dump)
				vertex_index_base = 0;
#endif

#if MESHDUMP_DEBUG
			obj_str += fmt::format("# base is %d\n", vertex_index_base);
#endif

			u32 min_idx = *std::min_element(d.indices.begin(), d.indices.end());
#if MESHDUMP_DEBUG
			if (min_idx > 0)
				obj_str += fmt::format("# warning: min_idx is %d\n", min_idx);
#endif

			if (vertex_format == vertex_format_t::_28)
			{
				for (auto tri_idx = 0; tri_idx < d.indices.size() / 3; tri_idx++)
				{
					const auto f0 = vertex_index_base + d.indices[tri_idx * 3 + 0] - min_idx;
					const auto f1 = vertex_index_base + d.indices[tri_idx * 3 + 2] - min_idx;
					const auto f2 = vertex_index_base + d.indices[tri_idx * 3 + 1] - min_idx;
					obj_str += fmt::format("f %d/%d %d/%d %d/%d\n",
						f0, f0,
						f1, f1,
						f2, f2);
				}
			}
			else if (vertex_format == vertex_format_t::_36)
			{
				for (auto tri_idx = 0; tri_idx < d.indices.size() / 3; tri_idx++)
				{
					const auto f0 = vertex_index_base + d.indices[tri_idx * 3 + 0] - min_idx;
					const auto f1 = vertex_index_base + d.indices[tri_idx * 3 + 2] - min_idx;
					const auto f2 = vertex_index_base + d.indices[tri_idx * 3 + 1] - min_idx;
					obj_str += fmt::format("f %d/%d/%d %d/%d/%d %d/%d/%d\n",
						f0, f0, f0 - vertex_index_base_normal_offset,
						f1, f1, f1 - vertex_index_base_normal_offset,
						f2, f2, f2 - vertex_index_base_normal_offset);
				}
			}
			else if (vertex_format == vertex_format_t::sly4_14 || vertex_format == vertex_format_t::sly4_18 || vertex_format == vertex_format_t::sly4_24)
			{
				for (auto tri_idx = 0; tri_idx < d.indices.size() / 3; tri_idx++)
				{
					const auto f0 = vertex_index_base + d.indices[tri_idx * 3 + 0] - min_idx;
					const auto f1 = vertex_index_base + d.indices[tri_idx * 3 + 2] - min_idx;
					const auto f2 = vertex_index_base + d.indices[tri_idx * 3 + 1] - min_idx;
					obj_str += fmt::format("f %d/%d %d/%d %d/%d\n",
						f0, f0,
						f1, f1,
						f2, f2);
				}
			}

			if ((u64)d.texture_raw_data_ptr != 0)
				g_dump_texture_info[(u64)d.texture_raw_data_ptr].is_used = true;
		}
		else
		{
			obj_str += fmt::format("# warning: unsupported, no vertices will be emitted\n");
		}

#if !MESHDUMP_NOCLIP || MESHDUMP_BATCH_DUMPS
		vertex_index_base += vertex_count;
#endif
	}

	file_obj.write(obj_str.c_str(), obj_str.size());

	dump_index++;

	g_dump_texture_info.clear();
	g_mesh_dumper.enabled = false;
	//g_mesh_dumper.enable_this_frame = false;
	//g_mesh_dumper.enable_this_frame2 = false;
	g_mesh_dumper.dumps.clear();

	Emu.Resume();
}