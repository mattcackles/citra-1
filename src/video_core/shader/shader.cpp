// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <memory>
#include <unordered_map>

#include "common/hash.h"
#include "common/make_unique.h"
#include "common/profiler.h"

#include "video_core/debug_utils/debug_utils.h"
#include "video_core/pica.h"
#include "video_core/video_core.h"

#include "shader.h"
#include "shader_interpreter.h"

#ifdef ARCHITECTURE_x86_64
#include "shader_jit_x64.h"
#endif // ARCHITECTURE_x86_64

namespace Pica {

namespace Shader {

#ifdef ARCHITECTURE_x86_64
static std::unordered_map<u64, CompiledShader*> shader_map;
static JitCompiler jit;
static CompiledShader* jit_shader;
#endif // ARCHITECTURE_x86_64

void Setup(UnitState& state) {
#ifdef ARCHITECTURE_x86_64
    if (VideoCore::g_shader_jit_enabled) {
        u64 cache_key = (Common::ComputeHash64(&g_state.vs.program_code, sizeof(g_state.vs.program_code)) ^
            Common::ComputeHash64(&g_state.vs.swizzle_data, sizeof(g_state.vs.swizzle_data)) ^
            g_state.regs.vs.main_offset);

        auto iter = shader_map.find(cache_key);
        if (iter != shader_map.end()) {
            jit_shader = iter->second;
        } else {
            jit_shader = jit.Compile();
            shader_map.emplace(cache_key, jit_shader);
        }
    }
#endif // ARCHITECTURE_x86_64
}

void Shutdown() {
    shader_map.clear();
}

static Common::Profiling::TimingCategory shader_category("Vertex Shader");

OutputVertex Run(UnitState& state, const InputVertex& input, int num_attributes) {
    auto& config = g_state.regs.vs;
    auto& setup = g_state.vs;

    Common::Profiling::ScopeTimer timer(shader_category);

    state.program_counter = config.main_offset;
    state.debug.max_offset = 0;
    state.debug.max_opdesc_id = 0;

    // Setup input register table
    const auto& attribute_register_map = config.input_register_map;

    if (num_attributes > 0) state.registers.input[attribute_register_map.attribute0_register] = input.attr[0];
    if (num_attributes > 1) state.registers.input[attribute_register_map.attribute1_register] = input.attr[1];
    if (num_attributes > 2) state.registers.input[attribute_register_map.attribute2_register] = input.attr[2];
    if (num_attributes > 3) state.registers.input[attribute_register_map.attribute3_register] = input.attr[3];
    if (num_attributes > 4) state.registers.input[attribute_register_map.attribute4_register] = input.attr[4];
    if (num_attributes > 5) state.registers.input[attribute_register_map.attribute5_register] = input.attr[5];
    if (num_attributes > 6) state.registers.input[attribute_register_map.attribute6_register] = input.attr[6];
    if (num_attributes > 7) state.registers.input[attribute_register_map.attribute7_register] = input.attr[7];
    if (num_attributes > 8) state.registers.input[attribute_register_map.attribute8_register] = input.attr[8];
    if (num_attributes > 9) state.registers.input[attribute_register_map.attribute9_register] = input.attr[9];
    if (num_attributes > 10) state.registers.input[attribute_register_map.attribute10_register] = input.attr[10];
    if (num_attributes > 11) state.registers.input[attribute_register_map.attribute11_register] = input.attr[11];
    if (num_attributes > 12) state.registers.input[attribute_register_map.attribute12_register] = input.attr[12];
    if (num_attributes > 13) state.registers.input[attribute_register_map.attribute13_register] = input.attr[13];
    if (num_attributes > 14) state.registers.input[attribute_register_map.attribute14_register] = input.attr[14];
    if (num_attributes > 15) state.registers.input[attribute_register_map.attribute15_register] = input.attr[15];

    state.conditional_code[0] = false;
    state.conditional_code[1] = false;

#ifdef ARCHITECTURE_x86_64
    if (VideoCore::g_shader_jit_enabled)
        jit_shader(&state.registers);
    else
        RunInterpreter(state);
#else
    RunInterpreter(state);
#endif // ARCHITECTURE_x86_64

#if PICA_DUMP_SHADERS
    DebugUtils::DumpShader(setup.program_code.data(), state.debug.max_offset, setup.swizzle_data.data(),
        state.debug.max_opdesc_id, config.main_offset,
        g_state.regs.vs_output_attributes); // TODO: Don't hardcode VS here
#endif

    // Setup output data
    OutputVertex ret;
    // TODO(neobrain): Under some circumstances, up to 16 attributes may be output. We need to
    // figure out what those circumstances are and enable the remaining outputs then.
    for (int i = 0; i < 7; ++i) {
        const auto& output_register_map = g_state.regs.vs_output_attributes[i]; // TODO: Don't hardcode VS here

        u32 semantics[4] = {
            output_register_map.map_x, output_register_map.map_y,
            output_register_map.map_z, output_register_map.map_w
        };

        for (int comp = 0; comp < 4; ++comp) {
            float24* out = ((float24*)&ret) + semantics[comp];
            if (semantics[comp] != Regs::VSOutputAttributes::INVALID) {
                *out = state.registers.output[i][comp];
            } else {
                // Zero output so that attributes which aren't output won't have denormals in them,
                // which would slow us down later.
                memset(out, 0, sizeof(*out));
            }
        }
    }

    // The hardware takes the absolute and saturates vertex colors like this, *before* doing interpolation
    for (int i = 0; i < 4; ++i) {
        ret.color[i] = float24::FromFloat32(
            std::fmin(std::fabs(ret.color[i].ToFloat32()), 1.0f));
    }

    LOG_TRACE(Render_Software, "Output vertex: pos (%.2f, %.2f, %.2f, %.2f), col(%.2f, %.2f, %.2f, %.2f), tc0(%.2f, %.2f)",
        ret.pos.x.ToFloat32(), ret.pos.y.ToFloat32(), ret.pos.z.ToFloat32(), ret.pos.w.ToFloat32(),
        ret.color.x.ToFloat32(), ret.color.y.ToFloat32(), ret.color.z.ToFloat32(), ret.color.w.ToFloat32(),
        ret.tc0.u().ToFloat32(), ret.tc0.v().ToFloat32());

    return ret;
}

} // namespace Shader

} // namespace Pica
