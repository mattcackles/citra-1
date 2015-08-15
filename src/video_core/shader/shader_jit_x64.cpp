// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <smmintrin.h>

#include "common/x64/abi.h"
#include "common/x64/cpu_detect.h"
#include "common/x64/emitter.h"

#include "shader.h"
#include "shader_jit_x64.h"

namespace Pica {

namespace Shader {

using namespace Gen;

typedef void (JitCompiler::*JitFunction)(Instruction instr);

const JitFunction instr_table[64] = {
    &JitCompiler::Compile_ADD,      // add
    &JitCompiler::Compile_DP3,      // dp3
    &JitCompiler::Compile_DP4,      // dp4
    nullptr,                        // dph
    nullptr,                        // unknown
    nullptr,                        // ex2
    nullptr,                        // lg2
    nullptr,                        // unknown
    &JitCompiler::Compile_MUL,      // mul
    nullptr,                        // lge
    nullptr,                        // slt
    &JitCompiler::Compile_FLR,      // flr
    &JitCompiler::Compile_MAX,      // max
    &JitCompiler::Compile_MIN,      // min
    &JitCompiler::Compile_RCP,      // rcp
    &JitCompiler::Compile_RSQ,      // rsq
    nullptr,                        // unknown
    nullptr,                        // unknown
    &JitCompiler::Compile_MOVA,     // mova
    &JitCompiler::Compile_MOV,      // mov
    nullptr,                        // unknown
    nullptr,                        // unknown
    nullptr,                        // unknown
    nullptr,                        // unknown
    nullptr,                        // dphi
    nullptr,                        // unknown
    nullptr,                        // sgei
    &JitCompiler::Compile_SLTI,     // slti
    nullptr,                        // unknown
    nullptr,                        // unknown
    nullptr,                        // unknown
    nullptr,                        // unknown
    nullptr,                        // unknown
    &JitCompiler::Compile_NOP,      // nop
    &JitCompiler::Compile_END,      // end
    nullptr,                        // break
    &JitCompiler::Compile_CALL,     // call
    &JitCompiler::Compile_CALLC,    // callc
    &JitCompiler::Compile_CALLU,    // callu
    &JitCompiler::Compile_IF,       // ifu
    &JitCompiler::Compile_IF,       // ifc
    &JitCompiler::Compile_LOOP,     // loop
    nullptr,                        // emit
    nullptr,                        // sete
    &JitCompiler::Compile_JMP,      // jmpc
    &JitCompiler::Compile_JMP,      // jmpu
    &JitCompiler::Compile_CMP,      // cmp
    &JitCompiler::Compile_CMP,      // cmp
    &JitCompiler::Compile_MAD,      // madi
    &JitCompiler::Compile_MAD,      // madi
    &JitCompiler::Compile_MAD,      // madi
    &JitCompiler::Compile_MAD,      // madi
    &JitCompiler::Compile_MAD,      // madi
    &JitCompiler::Compile_MAD,      // madi
    &JitCompiler::Compile_MAD,      // madi
    &JitCompiler::Compile_MAD,      // madi
    &JitCompiler::Compile_MAD,      // mad
    &JitCompiler::Compile_MAD,      // mad
    &JitCompiler::Compile_MAD,      // mad
    &JitCompiler::Compile_MAD,      // mad
    &JitCompiler::Compile_MAD,      // mad
    &JitCompiler::Compile_MAD,      // mad
    &JitCompiler::Compile_MAD,      // mad
    &JitCompiler::Compile_MAD,      // mad
};

// The following is used to alias some commonly used registers. Generally, RAX-RDX and XMM0-XMM3 can
// be used as scratch registers within a compiler function. The other registers have designated
// purposes, as documented below:

/// Pointer to the uniform memory
static const X64Reg UNIFORMS = R9;
/// The two 32-bit VS address offset registers set by the MOVA instruction
static const X64Reg ADDROFFS_REG_0 = R10;
static const X64Reg ADDROFFS_REG_1 = R11;
/// VS loop count register
static const X64Reg LOOPCOUNT_REG = R12;
/// Current VS loop iteration number (we could probably use LOOPCOUNT_REG, but this quicker)
static const X64Reg LOOPCOUNT = RSI;
/// Number to increment LOOPCOUNT_REG by on each loop iteration
static const X64Reg LOOPINC = RDI;
/// Result of the previous CMP instruction for the X-component comparison
static const X64Reg COND0 = R13;
/// Result of the previous CMP instruction for the Y-component comparison
static const X64Reg COND1 = R14;
/// Pointer to the UnitState instance for the current VS unit
static const X64Reg REGISTERS = R15;
/// SIMD scratch register
static const X64Reg SCRATCH = XMM0;
/// Loaded with the first swizzled source register, otherwise can be used as a scratch register
static const X64Reg SRC1 = XMM1;
/// Loaded with the second swizzled source register, otherwise can be used as a scratch register
static const X64Reg SRC2 = XMM2;
/// Loaded with the third swizzled source register, otherwise can be used as a scratch register
static const X64Reg SRC3 = XMM3;
/// Constant vector of [1.0f, 1.0f, 1.0f, 1.0f], used to efficiently set a vector to one
static const X64Reg ONE = XMM14;
/// Constant vector of [-0.f, -0.f, -0.f, -0.f], used to efficiently negate a vector with XOR
static const X64Reg NEGBIT = XMM15;

/// Raw constant for the source register selector that indicates no swizzling is performed
static const u8 NO_SRC_REG_SWIZZLE = 0x1b;
/// Raw constant for the destination register enable mask that indicates all components are enabled
static const u8 NO_DEST_REG_MASK = 0xf;

/**
 * Loads and swizzles a source register into the specified XMM register.
 * @param instr VS instruction, used for determining how to load the source register
 * @param src_num Number indicating which source register to load (1 = src1, 2 = src2, 3 = src3)
 * @param src_reg SourceRegister object corresponding to the source register to load
 * @param dest Destination XMM register to store the loaded, swizzled source register
 */
void JitCompiler::Compile_SwizzleSrc(Instruction instr, unsigned src_num, SourceRegister src_reg, X64Reg dest) {
    X64Reg src_ptr;
    int src_offset;

    if (src_reg.GetRegisterType() == RegisterType::FloatUniform) {
        src_ptr = UNIFORMS;
        src_offset = src_reg.GetIndex() * sizeof(float24) * 4;
    } else {
        src_ptr = REGISTERS;
        src_offset = UnitState::InputOffset(src_reg);
    }

    unsigned operand_desc_id;
    if (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MAD ||
        instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI) {
        // The MAD and MADI instructions do not use the address offset registers, so loading the
        // source is a bit simpler here

        operand_desc_id = instr.mad.operand_desc_id;

        // Load the source
        MOVAPS(dest, MDisp(src_ptr, src_offset));
    } else {
        operand_desc_id = instr.common.operand_desc_id;

        const bool is_inverted = (0 != (instr.opcode.Value().GetInfo().subtype & OpCode::Info::SrcInversed));
        unsigned offset_src = is_inverted ? 2 : 1;

        if (src_num == offset_src && instr.common.address_register_index != 0) {
            switch (instr.common.address_register_index) {
            case 1: // address offset 1
                MOVAPS(dest, MComplex(src_ptr, ADDROFFS_REG_0, 1, src_offset));
                break;
            case 2: // address offset 2
                MOVAPS(dest, MComplex(src_ptr, ADDROFFS_REG_1, 1, src_offset));
                break;
            case 3: // adddress offet 3
                MOVAPS(dest, MComplex(src_ptr, LOOPCOUNT_REG, 1, src_offset));
                break;
            default:
                UNREACHABLE();
                break;
            }
        } else {
            // Load the source
            MOVAPS(dest, MDisp(src_ptr, src_offset));
        }
    }

    SwizzlePattern swiz = { g_state.vs.swizzle_data[operand_desc_id] };

    // Generate instructions for source register swizzling as needed
    u8 sel = swiz.GetRawSelector(src_num);
    if (sel != NO_SRC_REG_SWIZZLE) {
        // Selector component order needs to be reversed for the SHUFPS instruction
        sel = ((sel & 0xc0) >> 6) | ((sel & 3) << 6) | ((sel & 0xc) << 2) | ((sel & 0x30) >> 2);

        // Shuffle inputs for swizzle
        SHUFPS(dest, R(dest), sel);
    }

    // If the source register should be negated, flip the negative bit using XOR
    const bool negate[] = { swiz.negate_src1, swiz.negate_src2, swiz.negate_src3 };
    if (negate[src_num - 1]) {
        XORPS(dest, R(NEGBIT));
    }
}

void JitCompiler::Compile_DestEnable(Instruction instr,X64Reg src) {
    DestRegister dest;
    unsigned operand_desc_id;
    if (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MAD ||
        instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI) {
        operand_desc_id = instr.mad.operand_desc_id;
        dest = instr.mad.dest.Value();
    } else {
        operand_desc_id = instr.common.operand_desc_id;
        dest = instr.common.dest.Value();
    }

    SwizzlePattern swiz = { g_state.vs.swizzle_data[operand_desc_id] };

    // If all components are enabled, write the result to the destination register
    if (swiz.dest_mask == NO_DEST_REG_MASK) {
        // Store dest back to memory
        MOVAPS(MDisp(REGISTERS, UnitState::OutputOffset(dest)), src);

    } else {
        // Not all components are enabled, so mask the result when storing to the destination register...
        MOVAPS(SCRATCH, MDisp(REGISTERS, UnitState::OutputOffset(dest)));

        if (Common::GetCPUCaps().sse4_1) {
            u8 mask = ((swiz.dest_mask & 1) << 3) | ((swiz.dest_mask & 8) >> 3) | ((swiz.dest_mask & 2) << 1) | ((swiz.dest_mask & 4) >> 1);
            BLENDPS(SCRATCH, R(src), mask);
        } else {
            MOVAPS(XMM4, R(src));
            UNPCKHPS(XMM4, R(SCRATCH)); // Unpack X/Y components of source and destination
            UNPCKLPS(SCRATCH, R(src)); // Unpack Z/W components of source and destination

            // Compute selector to selectively copy source components to destination for SHUFPS instruction
            u8 sel = ((swiz.DestComponentEnabled(0) ? 1 : 0) << 0) |
                     ((swiz.DestComponentEnabled(1) ? 3 : 2) << 2) |
                     ((swiz.DestComponentEnabled(2) ? 0 : 1) << 4) |
                     ((swiz.DestComponentEnabled(3) ? 2 : 3) << 6);
            SHUFPS(SCRATCH, R(XMM4), sel);
        }

        // Store dest back to memory
        MOVAPS(MDisp(REGISTERS, UnitState::OutputOffset(dest)), SCRATCH);
    }
}

void JitCompiler::Compile_EvaluateCondition(Instruction instr) {
    // Note: NXOR is used below to check for equality
    switch (instr.flow_control.op) {
    case Instruction::FlowControlType::Or:
        MOV(32, R(RAX), R(COND0));
        MOV(32, R(RBX), R(COND1));
        XOR(32, R(RAX), Imm32(instr.flow_control.refx.Value() ^ 1));
        XOR(32, R(RBX), Imm32(instr.flow_control.refy.Value() ^ 1));
        OR(32, R(RAX), R(RBX));
        break;

    case Instruction::FlowControlType::And:
        MOV(32, R(RAX), R(COND0));
        MOV(32, R(RBX), R(COND1));
        XOR(32, R(RAX), Imm32(instr.flow_control.refx.Value() ^ 1));
        XOR(32, R(RBX), Imm32(instr.flow_control.refy.Value() ^ 1));
        AND(32, R(RAX), R(RBX));
        break;

    case Instruction::FlowControlType::JustX:
        MOV(32, R(RAX), R(COND0));
        XOR(32, R(RAX), Imm32(instr.flow_control.refx.Value() ^ 1));
        break;

    case Instruction::FlowControlType::JustY:
        MOV(32, R(RAX), R(COND1));
        XOR(32, R(RAX), Imm32(instr.flow_control.refy.Value() ^ 1));
        break;
    }
}

void JitCompiler::Compile_UniformCondition(Instruction instr) {
    int offset = offsetof(decltype(g_state.vs.uniforms), b) + (instr.flow_control.bool_uniform_id * sizeof(bool));
    CMP(sizeof(bool) * 8, MDisp(UNIFORMS, offset), Imm8(0));
}

void JitCompiler::Compile_ADD(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);
    ADDPS(SRC1, R(SRC2));
    Compile_DestEnable(instr, SRC1);
}

void JitCompiler::Compile_DP3(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);

    if (Common::GetCPUCaps().sse4_1) {
        DPPS(SRC1, R(SRC2), 0x7f);
    } else {
        MULPS(SRC1, R(SRC2));

        MOVAPS(SRC2, R(SRC1));
        SHUFPS(SRC2, R(SRC2), _MM_SHUFFLE(1, 1, 1, 1));

        MOVAPS(SRC3, R(SRC1));
        SHUFPS(SRC3, R(SRC3), _MM_SHUFFLE(2, 2, 2, 2));

        SHUFPS(SRC1, R(SRC1), _MM_SHUFFLE(0, 0, 0, 0));
        ADDPS(SRC1, R(SRC2));
        ADDPS(SRC1, R(SRC3));
    }

    Compile_DestEnable(instr, SRC1);
}

void JitCompiler::Compile_DP4(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);

    if (Common::GetCPUCaps().sse4_1) {
        DPPS(SRC1, R(SRC2), 0xff);
    } else {
        MULPS(SRC1, R(SRC2));

        MOVAPS(SRC2, R(SRC1));
        SHUFPS(SRC1, R(SRC1), _MM_SHUFFLE(2, 3, 0, 1)); // XYZW -> ZWXY
        ADDPS(SRC1, R(SRC2));

        MOVAPS(SRC2, R(SRC1));
        SHUFPS(SRC1, R(SRC1), _MM_SHUFFLE(0, 1, 2, 3)); // XYZW -> WZYX
        ADDPS(SRC1, R(SRC2));
    }

    Compile_DestEnable(instr, SRC1);
}

void JitCompiler::Compile_MUL(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);
    MULPS(SRC1, R(SRC2));
    Compile_DestEnable(instr, SRC1);
}

void JitCompiler::Compile_FLR(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);

    if (Common::GetCPUCaps().sse4_1) {
        ROUNDFLOORPS(SRC1, R(SRC1));
    } else {
        CVTPS2DQ(SRC1, R(SRC1));
        CVTDQ2PS(SRC1, R(SRC1));
    }

    Compile_DestEnable(instr, SRC1);
}

void JitCompiler::Compile_MAX(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);
    MAXPS(SRC1, R(SRC2));
    Compile_DestEnable(instr, SRC1);
}

void JitCompiler::Compile_MIN(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);
    MINPS(SRC1, R(SRC2));
    Compile_DestEnable(instr, SRC1);
}

void JitCompiler::Compile_MOVA(Instruction instr) {
    SwizzlePattern swiz = { g_state.vs.swizzle_data[instr.common.operand_desc_id] };

    if (!swiz.DestComponentEnabled(0) && !swiz.DestComponentEnabled(1)) {
        return; // NoOp
    }

    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);

    // Convert floats to integers (only care about X and Y components)
    CVTPS2DQ(SRC1, R(SRC1));

    // Get result
    MOVQ_xmm(R(RAX), SRC1);

    // Handle destination enable
    if (swiz.DestComponentEnabled(0) && swiz.DestComponentEnabled(1)) {
        // Move and sign-extend low 32 bits
        MOVSX(64, 32, ADDROFFS_REG_0, R(RAX));

        // Move and sign-extend high 32 bits
        SHR(64, R(RAX), Imm8(32));
        MOVSX(64, 32, ADDROFFS_REG_1, R(RAX));

        // Multiply by 16 to be used as an offset later
        SHL(64, R(ADDROFFS_REG_0), Imm8(4));
        SHL(64, R(ADDROFFS_REG_1), Imm8(4));
    } else {
        if (swiz.DestComponentEnabled(0)) {
            // Move and sign-extend low 32 bits
            MOVSX(64, 32, ADDROFFS_REG_0, R(RAX));

            // Multiply by 16 to be used as an offset later
            SHL(64, R(ADDROFFS_REG_0), Imm8(4));
        } else if (swiz.DestComponentEnabled(1)) {
            // Move and sign-extend high 32 bits
            SHR(64, R(RAX), Imm8(32));
            MOVSX(64, 32, ADDROFFS_REG_1, R(RAX));

            // Multiply by 16 to be used as an offset later
            SHL(64, R(ADDROFFS_REG_1), Imm8(4));
        }
    }
}

void JitCompiler::Compile_MOV(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_DestEnable(instr, SRC1);
}

void JitCompiler::Compile_SLTI(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1i, SRC1);
    Compile_SwizzleSrc(instr, 1, instr.common.src2i, SRC2);

    CMPSS(SRC1, R(SRC2), CMP_LT);
    ANDPS(SRC1, R(ONE));

    Compile_DestEnable(instr, SRC1);
}

void JitCompiler::Compile_RCP(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);

    // TODO(bunnei): RCPPS is a pretty rough approximation, this might cause problems if Pica
    // performs this operation more accurately. This should be checked on hardware.
    RCPPS(SRC1, R(SRC1));

    Compile_DestEnable(instr, SRC1);
}

void JitCompiler::Compile_RSQ(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);

    // TODO(bunnei): RSQRTPS is a pretty rough approximation, this might cause problems if Pica
    // performs this operation more accurately. This should be checked on hardware.
    RSQRTPS(SRC1, R(SRC1));

    Compile_DestEnable(instr, SRC1);
}

void JitCompiler::Compile_NOP(Instruction instr) {
}

void JitCompiler::Compile_END(Instruction instr) {
    ABI_PopAllCalleeSavedRegsAndAdjustStack();
    RET();
}

void JitCompiler::Compile_CALL(Instruction instr) {
    unsigned offset = instr.flow_control.dest_offset;
    while (offset < (instr.flow_control.dest_offset + instr.flow_control.num_instructions)) {
        Compile_NextInstr(&offset);
    }
}

void JitCompiler::Compile_CALLC(Instruction instr) {
    Compile_EvaluateCondition(instr);
    FixupBranch b = J_CC(CC_Z, true);
    Compile_CALL(instr);
    SetJumpTarget(b);
}

void JitCompiler::Compile_CALLU(Instruction instr) {
    Compile_UniformCondition(instr);
    FixupBranch b = J_CC(CC_Z, true);
    Compile_CALL(instr);
    SetJumpTarget(b);
}

void JitCompiler::Compile_CMP(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);

    static const u8 cmp[] = { CMP_EQ, CMP_NEQ, CMP_LT, CMP_LE, CMP_NLE, CMP_NLT };

    if (instr.common.compare_op.x == instr.common.compare_op.y) {
        // Compare X-component and Y-component together
        CMPPS(SRC1, R(SRC2), cmp[instr.common.compare_op.x]);

        MOVQ_xmm(R(COND0), SRC1);
        MOV(64, R(COND1), R(COND0));
    } else {
        // Compare X-component
        MOVAPS(SCRATCH, R(SRC1));
        CMPSS(SCRATCH, R(SRC2), cmp[instr.common.compare_op.x]);

        // Compare Y-component
        CMPPS(SRC1, R(SRC2), cmp[instr.common.compare_op.y]);

        MOVQ_xmm(R(COND0), SCRATCH);
        MOVQ_xmm(R(COND1), SRC1);
    }

    SHR(32, R(COND0), Imm8(31));
    SHR(64, R(COND1), Imm8(63));
}

void JitCompiler::Compile_MAD(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.mad.src1, SRC1);

    if (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI) {
        Compile_SwizzleSrc(instr, 2, instr.mad.src2i, SRC2);
        Compile_SwizzleSrc(instr, 3, instr.mad.src3i, SRC3);
    } else {
        Compile_SwizzleSrc(instr, 2, instr.mad.src2, SRC2);
        Compile_SwizzleSrc(instr, 3, instr.mad.src3, SRC3);
    }

    if (Common::GetCPUCaps().fma) {
        VFMADD213PS(SRC1, SRC2, R(SRC3));
    } else {
        MULPS(SRC1, R(SRC2));
        ADDPS(SRC1, R(SRC3));
    }

    Compile_DestEnable(instr, SRC1);
}

void JitCompiler::Compile_IF(Instruction instr) {
    ASSERT_MSG(instr.flow_control.dest_offset > *offset_ptr, "Backwards if-statements not supported");

    // Evaluate the "IF" condition
    if (instr.opcode.Value() == OpCode::Id::IFU) {
        Compile_UniformCondition(instr);
    } else if (instr.opcode.Value() == OpCode::Id::IFC) {
        Compile_EvaluateCondition(instr);
    }
    FixupBranch b = J_CC(CC_Z, true);

    // Compile the code that corresponds to the condition evaluating as true
    Compile_Block(instr.flow_control.dest_offset - 1);

    // If there isn't an "ELSE" condition, we are done here
    if (instr.flow_control.num_instructions == 0) {
        SetJumpTarget(b);
        return;
    }

    FixupBranch b2 = J(true);

    SetJumpTarget(b);

    // This code corresponds to the "ELSE" condition
    // Comple the code that corresponds to the condition evaluating as false
    Compile_Block(instr.flow_control.dest_offset + instr.flow_control.num_instructions - 1);

    SetJumpTarget(b2);
}

void JitCompiler::Compile_LOOP(Instruction instr) {
    ASSERT_MSG(instr.flow_control.dest_offset > *offset_ptr, "Backwards loops not supported");
    ASSERT_MSG(!looping, "Nested loops not supported");

    looping = true;

    int offset = offsetof(decltype(g_state.vs.uniforms), i) + (instr.flow_control.int_uniform_id * sizeof(Math::Vec4<u8>));
    MOV(32, R(LOOPCOUNT), MDisp(UNIFORMS, offset));
    MOV(32, R(LOOPCOUNT_REG), R(LOOPCOUNT));
    SHR(32, R(LOOPCOUNT_REG), Imm8(8));
    AND(32, R(LOOPCOUNT_REG), Imm32(0xff)); // Y-component is the start
    MOV(32, R(LOOPINC), R(LOOPCOUNT));
    SHR(32, R(LOOPINC), Imm8(16));
    MOVZX(32, 8, LOOPINC, R(LOOPINC)); // Z-component is the incrementer
    MOVZX(32, 8, LOOPCOUNT, R(LOOPCOUNT)); // X-component is iteration count
    ADD(32, R(LOOPCOUNT), Imm8(1)); // Iteration count is X-component + 1

    auto loop_start = GetCodePtr();

    Compile_Block(instr.flow_control.dest_offset);

    ADD(32, R(LOOPCOUNT_REG), R(LOOPINC)); // Increment LOOPCOUNT_REG by Z-component
    SUB(32, R(LOOPCOUNT), Imm8(1)); // Increment loop count by 1
    J_CC(CC_NZ, loop_start); // Loop if not equal

    looping = false;
}

void JitCompiler::Compile_JMP(Instruction instr) {
    ASSERT_MSG(instr.flow_control.dest_offset > *offset_ptr, "Backwards jumps not supported");

    if (instr.opcode.Value() == OpCode::Id::JMPC)
        Compile_EvaluateCondition(instr);
    else if (instr.opcode.Value() == OpCode::Id::JMPU)
        Compile_UniformCondition(instr);
    else
        UNREACHABLE();

    FixupBranch b = J_CC(CC_NZ, true);

    Compile_Block(instr.flow_control.dest_offset);

    SetJumpTarget(b);
}

void JitCompiler::Compile_Block(unsigned stop) {
    // Save current offset pointer
    unsigned* prev_offset_ptr = offset_ptr;
    unsigned offset = *prev_offset_ptr;

    while (offset <= stop)
        Compile_NextInstr(&offset);

    // Restore current offset pointer
    offset_ptr = prev_offset_ptr;
    *offset_ptr = offset;
}

void JitCompiler::Compile_NextInstr(unsigned* offset) {
    offset_ptr = offset;

    Instruction instr = *(Instruction*)&g_state.vs.program_code[(*offset_ptr)++];
    OpCode::Id opcode = instr.opcode.Value();
    auto instr_func = instr_table[static_cast<unsigned>(opcode)];

    if (instr_func) {
        // JIT the instruction!
        ((*this).*instr_func)(instr);
    } else {
        // Unhandled instruction
        LOG_CRITICAL(HW_GPU, "Unhandled instruction: 0x%02x (0x%08x)", instr.opcode.Value(), instr.hex);
    }
}

CompiledShader* JitCompiler::Compile() {
    const u8* start = GetCodePtr();
    const auto& code = g_state.vs.program_code;
    unsigned offset = g_state.regs.vs.main_offset;

    ABI_PushAllCalleeSavedRegsAndAdjustStack();

    MOV(PTRBITS, R(REGISTERS), R(ABI_PARAM1));
    MOV(PTRBITS, R(UNIFORMS), ImmPtr(&g_state.vs.uniforms));

    // Zero address/loop  registers
    XOR(64, R(ADDROFFS_REG_0), R(ADDROFFS_REG_0));
    XOR(64, R(ADDROFFS_REG_1), R(ADDROFFS_REG_1));
    XOR(64, R(LOOPCOUNT_REG), R(LOOPCOUNT_REG));

    // Used to set a register to one
    static const __m128 one = { 1.f, 1.f, 1.f, 1.f };
    MOV(PTRBITS, R(RAX), ImmPtr(&one));
    MOVAPS(ONE, MDisp(RAX, 0));

    // Used to negate registers
    static const __m128 neg = { -0.f, -0.f, -0.f, -0.f };
    MOV(PTRBITS, R(RAX), ImmPtr(&neg));
    MOVAPS(NEGBIT, MDisp(RAX, 0));

    looping = false;

    while (offset < g_state.vs.program_code.size()) {
        Compile_NextInstr(&offset);
    }

    return (CompiledShader*)start;
}

JitCompiler::JitCompiler() {
    AllocCodeSpace(1024 * 1024 * 4);
}

void JitCompiler::Clear() {
    ClearCodeSpace();
}

} // namespace Shader

} // namespace Pica
