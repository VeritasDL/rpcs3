#pragma once

#include <array>
#include <vector>
#include <cstddef>
#include <map>
#include <mutex>

#include <Emu/RSX/RSXThread.h>
#include <Emu/RSX/linalg.h>

using namespace linalg::aliases;

struct vec2be
{
	be_t<float> u, v;
};
struct vec3be
{
	be_t<float> x, y, z;
};
struct uvec3
{
	u32 x_u, y_u, z_u;
};
struct vec4be
{
	be_t<float> x, y, z, w;
};
using vec2 = float2;
using vec3 = float3;
using vec4 = float4;
using mat4 = float4x4;
using mat4 = float4x4;

struct mesh_draw_dump_block
{
	std::vector<u8> vertex_data;
	rsx::interleaved_range_info interleaved_range_info;
};

using tex_raw_data_ptr_t = std::vector<std::byte>*;

struct mesh_draw_dump
{
	u32 clear_count; // Number of clears since frame start when this draw was dumped
	//std::vector<mesh_draw_vertex> vertices;
	//std::vector<u8> vertex_data;
	tex_raw_data_ptr_t texture_raw_data_ptr;
	std::vector<u8> volatile_data;
	std::vector<mesh_draw_dump_block> blocks;
	std::vector<u32> indices;
	std::array<float4, 468> vertex_constants_buffer;
	u32 vert_shader_hash;
	u32 frag_shader_hash;
};

struct mesh_dumper
{
	std::vector<mesh_draw_dump> dumps;

	bool enabled{};
	bool enable_this_frame{};
	bool enable_this_frame2{};

	void push_block(const mesh_draw_dump_block& block);
	void push_dump(const mesh_draw_dump& dump);
	mesh_draw_dump& get_dump();
	void dump();
};

struct texture_info_t
{
	u32 width;
	u32 height;
	u8 format; // CELL_GCM_TEXTURE_*
	bool is_used;
};

// key is tex_raw_data_ptr_t (can't actually use it)
extern std::map<u64, texture_info_t> g_dump_texture_info;

extern u32 g_clears_this_frame;

extern mesh_dumper g_mesh_dumper;
extern std::mutex g_mesh_dumper_mtx;
