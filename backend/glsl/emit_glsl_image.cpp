// Copyright 2021 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <string_view>

#include "shader_recompiler/backend/glsl/emit_context.h"
#include "shader_recompiler/backend/glsl/emit_glsl_instructions.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/profile.h"

namespace Shader::Backend::GLSL {
namespace {
std::string Texture(EmitContext& ctx, const IR::TextureInstInfo& info,
                    [[maybe_unused]] const IR::Value& index) {
    if (info.type == TextureType::Buffer) {
        return fmt::format("tex{}", ctx.texture_buffer_bindings.at(info.descriptor_index));
    } else {
        return fmt::format("tex{}", ctx.texture_bindings.at(info.descriptor_index));
    }
}

std::string Image(EmitContext& ctx, const IR::TextureInstInfo& info,
                  [[maybe_unused]] const IR::Value& index) {
    if (info.type == TextureType::Buffer) {
        return fmt::format("img{}", ctx.image_buffer_bindings.at(info.descriptor_index));
    } else {
        return fmt::format("img{}", ctx.image_bindings.at(info.descriptor_index));
    }
}

std::string CastToIntVec(std::string_view value, const IR::TextureInstInfo& info) {
    switch (info.type) {
    case TextureType::Color1D:
    case TextureType::Buffer:
        return fmt::format("int({})", value);
    case TextureType::ColorArray1D:
    case TextureType::Color2D:
    case TextureType::ColorArray2D:
        return fmt::format("ivec2({})", value);
    case TextureType::Color3D:
    case TextureType::ColorCube:
        return fmt::format("ivec3({})", value);
    case TextureType::ColorArrayCube:
        return fmt::format("ivec4({})", value);
    default:
        throw NotImplementedException("Offset type {}", info.type.Value());
    }
}

std::string TexelFetchCastToInt(std::string_view value, const IR::TextureInstInfo& info) {
    switch (info.type) {
    case TextureType::Color1D:
    case TextureType::Buffer:
        return fmt::format("int({})", value);
    case TextureType::ColorArray1D:
    case TextureType::Color2D:
        return fmt::format("ivec2({})", value);
    case TextureType::ColorArray2D:
    case TextureType::Color3D:
    case TextureType::ColorCube:
        return fmt::format("ivec3({})", value);
    case TextureType::ColorArrayCube:
        return fmt::format("ivec4({})", value);
    default:
        throw NotImplementedException("Offset type {}", info.type.Value());
    }
}

bool NeedsShadowLodExt(TextureType type) {
    switch (type) {
    case TextureType::ColorArray2D:
    case TextureType::ColorCube:
    case TextureType::ColorArrayCube:
        return true;
    default:
        return false;
    }
}

std::string GetOffsetVec(EmitContext& ctx, const IR::Value& offset) {
    if (offset.IsImmediate()) {
        return fmt::format("int({})", offset.U32());
    }
    IR::Inst* const inst{offset.InstRecursive()};
    if (inst->AreAllArgsImmediates()) {
        switch (inst->GetOpcode()) {
        case IR::Opcode::CompositeConstructU32x2:
            return fmt::format("ivec2({},{})", inst->Arg(0).U32(), inst->Arg(1).U32());
        case IR::Opcode::CompositeConstructU32x3:
            return fmt::format("ivec3({},{},{})", inst->Arg(0).U32(), inst->Arg(1).U32(),
                               inst->Arg(2).U32());
        case IR::Opcode::CompositeConstructU32x4:
            return fmt::format("ivec4({},{},{},{})", inst->Arg(0).U32(), inst->Arg(1).U32(),
                               inst->Arg(2).U32(), inst->Arg(3).U32());
        default:
            break;
        }
    }
    return ctx.var_alloc.Consume(offset);
}

std::string PtpOffsets(const IR::Value& offset, const IR::Value& offset2) {
    const std::array values{offset.InstRecursive(), offset2.InstRecursive()};
    if (!values[0]->AreAllArgsImmediates() || !values[1]->AreAllArgsImmediates()) {
        // LOG_WARNING("Not all arguments in PTP are immediate, STUBBING");
        return "ivec2[](ivec2(0), ivec2(1), ivec2(2), ivec2(3))";
    }
    const IR::Opcode opcode{values[0]->GetOpcode()};
    if (opcode != values[1]->GetOpcode() || opcode != IR::Opcode::CompositeConstructU32x4) {
        throw LogicError("Invalid PTP arguments");
    }
    auto read{[&](unsigned int a, unsigned int b) { return values[a]->Arg(b).U32(); }};

    return fmt::format("ivec2[](ivec2({},{}),ivec2({},{}),ivec2({},{}),ivec2({},{}))", read(0, 0),
                       read(0, 1), read(0, 2), read(0, 3), read(1, 0), read(1, 1), read(1, 2),
                       read(1, 3));
}

IR::Inst* PrepareSparse(IR::Inst& inst) {
    const auto sparse_inst{inst.GetAssociatedPseudoOperation(IR::Opcode::GetSparseFromOp)};
    if (sparse_inst) {
        sparse_inst->Invalidate();
    }
    return sparse_inst;
}
} // Anonymous namespace

void EmitImageSampleImplicitLod([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                                [[maybe_unused]] const IR::Value& index,
                                [[maybe_unused]] std::string_view coords,
                                [[maybe_unused]] std::string_view bias_lc,
                                [[maybe_unused]] const IR::Value& offset) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    if (info.has_lod_clamp) {
        throw NotImplementedException("EmitImageSampleImplicitLod Lod clamp samples");
    }
    const auto texture{Texture(ctx, info, index)};
    const auto bias{info.has_bias ? fmt::format(",{}", bias_lc) : ""};
    const auto texel{ctx.var_alloc.Define(inst, GlslVarType::F32x4)};
    const auto sparse_inst{PrepareSparse(inst)};
    if (!sparse_inst) {
        if (!offset.IsEmpty()) {
            const auto offset_str{GetOffsetVec(ctx, offset)};
            if (ctx.stage == Stage::Fragment) {
                ctx.Add("{}=textureOffset({},{},{}{});", texel, texture, coords, offset_str, bias);
            } else {
                ctx.Add("{}=textureLodOffset({},{},0.0,{});", texel, texture, coords, offset_str);
            }
        } else {
            if (ctx.stage == Stage::Fragment) {
                ctx.Add("{}=texture({},{}{});", texel, texture, coords, bias);
            } else {
                ctx.Add("{}=textureLod({},{},0.0);", texel, texture, coords);
            }
        }
        return;
    }
    // TODO: Query sparseTexels extension support
    if (!offset.IsEmpty()) {
        ctx.AddU1("{}=sparseTexelsResidentARB(sparseTextureOffsetARB({},{},{},{}{}));",
                  *sparse_inst, texture, coords, GetOffsetVec(ctx, offset), texel, bias);
    } else {
        ctx.AddU1("{}=sparseTexelsResidentARB(sparseTextureARB({},{},{}{}));", *sparse_inst,
                  texture, coords, texel, bias);
    }
}

void EmitImageSampleExplicitLod([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                                [[maybe_unused]] const IR::Value& index,
                                [[maybe_unused]] std::string_view coords,
                                [[maybe_unused]] std::string_view lod_lc,
                                [[maybe_unused]] const IR::Value& offset) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    if (info.has_bias) {
        throw NotImplementedException("EmitImageSampleExplicitLod Bias texture samples");
    }
    if (info.has_lod_clamp) {
        throw NotImplementedException("EmitImageSampleExplicitLod Lod clamp samples");
    }
    const auto texture{Texture(ctx, info, index)};
    const auto texel{ctx.var_alloc.Define(inst, GlslVarType::F32x4)};
    const auto sparse_inst{PrepareSparse(inst)};
    if (!sparse_inst) {
        if (!offset.IsEmpty()) {
            ctx.Add("{}=textureLodOffset({},{},{},{});", texel, texture, coords, lod_lc,
                    GetOffsetVec(ctx, offset));
        } else {
            ctx.Add("{}=textureLod({},{},{});", texel, texture, coords, lod_lc);
        }
        return;
    }
    // TODO: Query sparseTexels extension support
    if (!offset.IsEmpty()) {
        ctx.AddU1("{}=sparseTexelsResidentARB(sparseTexelFetchOffsetARB({},{},int({}),{},{}));",
                  *sparse_inst, texture, CastToIntVec(coords, info), lod_lc,
                  GetOffsetVec(ctx, offset), texel);
    } else {
        ctx.AddU1("{}=sparseTexelsResidentARB(sparseTextureLodARB({},{},{},{}));", *sparse_inst,
                  texture, coords, lod_lc, texel);
    }
}

void EmitImageSampleDrefImplicitLod([[maybe_unused]] EmitContext& ctx,
                                    [[maybe_unused]] IR::Inst& inst,
                                    [[maybe_unused]] const IR::Value& index,
                                    [[maybe_unused]] std::string_view coords,
                                    [[maybe_unused]] std::string_view dref,
                                    [[maybe_unused]] std::string_view bias_lc,
                                    [[maybe_unused]] const IR::Value& offset) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const auto sparse_inst{PrepareSparse(inst)};
    if (sparse_inst) {
        throw NotImplementedException("EmitImageSampleDrefImplicitLod Sparse texture samples");
    }
    if (info.has_bias) {
        throw NotImplementedException("EmitImageSampleDrefImplicitLod Bias texture samples");
    }
    if (info.has_lod_clamp) {
        throw NotImplementedException("EmitImageSampleDrefImplicitLod Lod clamp samples");
    }
    const auto texture{Texture(ctx, info, index)};
    const auto bias{info.has_bias ? fmt::format(",{}", bias_lc) : ""};
    const bool needs_shadow_ext{NeedsShadowLodExt(info.type)};
    const auto cast{needs_shadow_ext ? "vec4" : "vec3"};
    const bool use_grad{!ctx.profile.support_gl_texture_shadow_lod &&
                        ctx.stage != Stage::Fragment && needs_shadow_ext};
    if (use_grad) {
        // LOG_WARNING(..., "Device lacks GL_EXT_texture_shadow_lod. Using textureGrad fallback");
        if (info.type == TextureType::ColorArrayCube) {
            // LOG_WARNING(..., "textureGrad does not support ColorArrayCube. Stubbing");
            ctx.AddF32("{}=0.0f;", inst);
            return;
        }
        const auto d_cast{info.type == TextureType::ColorArray2D ? "vec2" : "vec3"};
        ctx.AddF32("{}=textureGrad({},{}({},{}),{}(0),{}(0));", inst, texture, cast, coords, dref,
                   d_cast, d_cast);
        return;
    }
    if (!offset.IsEmpty()) {
        const auto offset_str{GetOffsetVec(ctx, offset)};
        if (ctx.stage == Stage::Fragment) {
            ctx.AddF32("{}=textureOffset({},{}({},{}),{}{});", inst, texture, cast, coords, dref,
                       offset_str, bias);
        } else {
            ctx.AddF32("{}=textureLodOffset({},{}({},{}),0.0,{});", inst, texture, cast, coords,
                       dref, offset_str);
        }
    } else {
        if (ctx.stage == Stage::Fragment) {
            if (info.type == TextureType::ColorArrayCube) {
                ctx.AddF32("{}=texture({},vec4({}),{});", inst, texture, coords, dref);
            } else {
                ctx.AddF32("{}=texture({},{}({},{}){});", inst, texture, cast, coords, dref, bias);
            }
        } else {
            ctx.AddF32("{}=textureLod({},{}({},{}),0.0);", inst, texture, cast, coords, dref);
        }
    }
}

void EmitImageSampleDrefExplicitLod([[maybe_unused]] EmitContext& ctx,
                                    [[maybe_unused]] IR::Inst& inst,
                                    [[maybe_unused]] const IR::Value& index,
                                    [[maybe_unused]] std::string_view coords,
                                    [[maybe_unused]] std::string_view dref,
                                    [[maybe_unused]] std::string_view lod_lc,
                                    [[maybe_unused]] const IR::Value& offset) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const auto sparse_inst{PrepareSparse(inst)};
    if (sparse_inst) {
        throw NotImplementedException("EmitImageSampleDrefExplicitLod Sparse texture samples");
    }
    if (info.has_bias) {
        throw NotImplementedException("EmitImageSampleDrefExplicitLod Bias texture samples");
    }
    if (info.has_lod_clamp) {
        throw NotImplementedException("EmitImageSampleDrefExplicitLod Lod clamp samples");
    }
    const auto texture{Texture(ctx, info, index)};
    const bool needs_shadow_ext{NeedsShadowLodExt(info.type)};
    const bool use_grad{!ctx.profile.support_gl_texture_shadow_lod && needs_shadow_ext};
    const auto cast{needs_shadow_ext ? "vec4" : "vec3"};
    if (use_grad) {
        // LOG_WARNING(..., "Device lacks GL_EXT_texture_shadow_lod. Using textureGrad fallback");
        if (info.type == TextureType::ColorArrayCube) {
            // LOG_WARNING(..., "textureGrad does not support ColorArrayCube. Stubbing");
            ctx.AddF32("{}=0.0f;", inst);
            return;
        }
        const auto d_cast{info.type == TextureType::ColorArray2D ? "vec2" : "vec3"};
        ctx.AddF32("{}=textureGrad({},{}({},{}),{}(0),{}(0));", inst, texture, cast, coords, dref,
                   d_cast, d_cast);
        return;
    }
    if (!offset.IsEmpty()) {
        const auto offset_str{GetOffsetVec(ctx, offset)};
        if (info.type == TextureType::ColorArrayCube) {
            ctx.AddF32("{}=textureLodOffset({},{},{},{},{});", inst, texture, coords, dref, lod_lc,
                       offset_str);
        } else {
            ctx.AddF32("{}=textureLodOffset({},{}({},{}),{},{});", inst, texture, cast, coords,
                       dref, lod_lc, offset_str);
        }
    } else {
        if (info.type == TextureType::ColorArrayCube) {
            ctx.AddF32("{}=textureLod({},{},{},{});", inst, texture, coords, dref, lod_lc);
        } else {
            ctx.AddF32("{}=textureLod({},{}({},{}),{});", inst, texture, cast, coords, dref,
                       lod_lc);
        }
    }
}

void EmitImageGather([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                     [[maybe_unused]] const IR::Value& index,
                     [[maybe_unused]] std::string_view coords,
                     [[maybe_unused]] const IR::Value& offset,
                     [[maybe_unused]] const IR::Value& offset2) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const auto texture{Texture(ctx, info, index)};
    const auto texel{ctx.var_alloc.Define(inst, GlslVarType::F32x4)};
    const auto sparse_inst{PrepareSparse(inst)};
    if (!sparse_inst) {
        if (offset.IsEmpty()) {
            ctx.Add("{}=textureGather({},{},int({}));", texel, texture, coords,
                    info.gather_component);
            return;
        }
        if (offset2.IsEmpty()) {
            ctx.Add("{}=textureGatherOffset({},{},{},int({}));", texel, texture, coords,
                    GetOffsetVec(ctx, offset), info.gather_component);
            return;
        }
        // PTP
        const auto offsets{PtpOffsets(offset, offset2)};
        ctx.Add("{}=textureGatherOffsets({},{},{},int({}));", texel, texture, coords, offsets,
                info.gather_component);
        return;
    }
    // TODO: Query sparseTexels extension support
    if (offset.IsEmpty()) {
        ctx.AddU1("{}=sparseTexelsResidentARB(sparseTextureGatherARB({},{},{},int({})));",
                  *sparse_inst, texture, coords, texel, info.gather_component);
        return;
    }
    if (offset2.IsEmpty()) {
        ctx.AddU1("{}=sparseTexelsResidentARB(sparseTextureGatherOffsetARB({},{},{},{},int({})));",
                  *sparse_inst, texture, CastToIntVec(coords, info), GetOffsetVec(ctx, offset),
                  texel, info.gather_component);
        return;
    }
    // PTP
    const auto offsets{PtpOffsets(offset, offset2)};
    ctx.AddU1("{}=sparseTexelsResidentARB(sparseTextureGatherOffsetARB({},{},{},{},int({})));",
              *sparse_inst, texture, CastToIntVec(coords, info), offsets, texel,
              info.gather_component);
}

void EmitImageGatherDref([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                         [[maybe_unused]] const IR::Value& index,
                         [[maybe_unused]] std::string_view coords,
                         [[maybe_unused]] const IR::Value& offset,
                         [[maybe_unused]] const IR::Value& offset2,
                         [[maybe_unused]] std::string_view dref) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const auto texture{Texture(ctx, info, index)};
    const auto texel{ctx.var_alloc.Define(inst, GlslVarType::F32x4)};
    const auto sparse_inst{PrepareSparse(inst)};
    if (!sparse_inst) {
        if (offset.IsEmpty()) {
            ctx.Add("{}=textureGather({},{},{});", texel, texture, coords, dref);
            return;
        }
        if (offset2.IsEmpty()) {
            ctx.Add("{}=textureGatherOffset({},{},{},{});", texel, texture, coords, dref,
                    GetOffsetVec(ctx, offset));
            return;
        }
        // PTP
        const auto offsets{PtpOffsets(offset, offset2)};
        ctx.Add("{}=textureGatherOffsets({},{},{},{});", texel, texture, coords, dref, offsets);
        return;
    }
    // TODO: Query sparseTexels extension support
    if (offset.IsEmpty()) {
        ctx.AddU1("{}=sparseTexelsResidentARB(sparseTextureGatherARB({},{},{},{}));", *sparse_inst,
                  texture, coords, dref, texel);
        return;
    }
    if (offset2.IsEmpty()) {
        ctx.AddU1("{}=sparseTexelsResidentARB(sparseTextureGatherOffsetARB({},{},{},,{},{}));",
                  *sparse_inst, texture, CastToIntVec(coords, info), dref,
                  GetOffsetVec(ctx, offset), texel);
        return;
    }
    // PTP
    const auto offsets{PtpOffsets(offset, offset2)};
    ctx.AddU1("{}=sparseTexelsResidentARB(sparseTextureGatherOffsetARB({},{},{},,{},{}));",
              *sparse_inst, texture, CastToIntVec(coords, info), dref, offsets, texel);
}

void EmitImageFetch([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                    [[maybe_unused]] const IR::Value& index,
                    [[maybe_unused]] std::string_view coords,
                    [[maybe_unused]] std::string_view offset, [[maybe_unused]] std::string_view lod,
                    [[maybe_unused]] std::string_view ms) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    if (info.has_bias) {
        throw NotImplementedException("EmitImageFetch Bias texture samples");
    }
    if (info.has_lod_clamp) {
        throw NotImplementedException("EmitImageFetch Lod clamp samples");
    }
    const auto texture{Texture(ctx, info, index)};
    const auto sparse_inst{PrepareSparse(inst)};
    const auto texel{ctx.var_alloc.Define(inst, GlslVarType::F32x4)};
    if (!sparse_inst) {
        if (!offset.empty()) {
            ctx.Add("{}=texelFetchOffset({},{},int({}),{});", texel, texture,
                    TexelFetchCastToInt(coords, info), lod, TexelFetchCastToInt(offset, info));
        } else {
            if (info.type == TextureType::Buffer) {
                ctx.Add("{}=texelFetch({},int({}));", texel, texture, coords);
            } else {
                ctx.Add("{}=texelFetch({},{},int({}));", texel, texture,
                        TexelFetchCastToInt(coords, info), lod);
            }
        }
        return;
    }
    // TODO: Query sparseTexels extension support
    if (!offset.empty()) {
        ctx.AddU1("{}=sparseTexelsResidentARB(sparseTexelFetchOffsetARB({},{},int({}),{},{}));",
                  *sparse_inst, texture, CastToIntVec(coords, info), lod,
                  CastToIntVec(offset, info), texel);
    } else {
        ctx.AddU1("{}=sparseTexelsResidentARB(sparseTexelFetchARB({},{},int({}),{}));",
                  *sparse_inst, texture, CastToIntVec(coords, info), lod, texel);
    }
}

void EmitImageQueryDimensions([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                              [[maybe_unused]] const IR::Value& index,
                              [[maybe_unused]] std::string_view lod) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const auto texture{Texture(ctx, info, index)};
    switch (info.type) {
    case TextureType::Color1D:
        return ctx.AddU32x4(
            "{}=uvec4(uint(textureSize({},int({}))),0u,0u,uint(textureQueryLevels({})));", inst,
            texture, lod, texture);
    case TextureType::ColorArray1D:
    case TextureType::Color2D:
    case TextureType::ColorCube:
        return ctx.AddU32x4(
            "{}=uvec4(uvec2(textureSize({},int({}))),0u,uint(textureQueryLevels({})));", inst,
            texture, lod, texture);
    case TextureType::ColorArray2D:
    case TextureType::Color3D:
    case TextureType::ColorArrayCube:
        return ctx.AddU32x4(
            "{}=uvec4(uvec3(textureSize({},int({}))),uint(textureQueryLevels({})));", inst, texture,
            lod, texture);
    case TextureType::Buffer:
        throw NotImplementedException("EmitImageQueryDimensions Texture buffers");
    }
    throw LogicError("Unspecified image type {}", info.type.Value());
}

void EmitImageQueryLod([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                       [[maybe_unused]] const IR::Value& index,
                       [[maybe_unused]] std::string_view coords) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const auto texture{Texture(ctx, info, index)};
    return ctx.AddF32x4("{}=vec4(textureQueryLod({},{}),0.0,0.0);", inst, texture, coords);
}

void EmitImageGradient([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                       [[maybe_unused]] const IR::Value& index,
                       [[maybe_unused]] std::string_view coords,
                       [[maybe_unused]] const IR::Value& derivatives,
                       [[maybe_unused]] const IR::Value& offset,
                       [[maybe_unused]] const IR::Value& lod_clamp) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    if (info.has_lod_clamp) {
        throw NotImplementedException("EmitImageGradient Lod clamp samples");
    }
    const auto sparse_inst{PrepareSparse(inst)};
    if (sparse_inst) {
        throw NotImplementedException("EmitImageGradient Sparse");
    }
    if (!offset.IsEmpty()) {
        throw NotImplementedException("EmitImageGradient offset");
    }
    const auto texture{Texture(ctx, info, index)};
    const auto texel{ctx.var_alloc.Define(inst, GlslVarType::F32x4)};
    const bool multi_component{info.num_derivates > 1 || info.has_lod_clamp};
    const auto derivatives_vec{ctx.var_alloc.Consume(derivatives)};
    if (multi_component) {
        ctx.Add("{}=textureGrad({},{},vec2({}.xz),vec2({}.yz));", texel, texture, coords,
                derivatives_vec, derivatives_vec);
    } else {
        ctx.Add("{}=textureGrad({},{},float({}.x),float({}.y));", texel, texture, coords,
                derivatives_vec, derivatives_vec);
    }
}

void EmitImageRead([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                   [[maybe_unused]] const IR::Value& index,
                   [[maybe_unused]] std::string_view coords) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const auto sparse_inst{PrepareSparse(inst)};
    if (sparse_inst) {
        throw NotImplementedException("EmitImageRead Sparse");
    }
    const auto image{Image(ctx, info, index)};
    ctx.AddU32x4("{}=uvec4(imageLoad({},{}));", inst, image, TexelFetchCastToInt(coords, info));
}

void EmitImageWrite([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                    [[maybe_unused]] const IR::Value& index,
                    [[maybe_unused]] std::string_view coords,
                    [[maybe_unused]] std::string_view color) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const auto image{Image(ctx, info, index)};
    ctx.Add("imageStore({},{},{});", image, TexelFetchCastToInt(coords, info), color);
}

void EmitBindlessImageSampleImplicitLod(EmitContext&) {
    NotImplemented();
}

void EmitBindlessImageSampleExplicitLod(EmitContext&) {
    NotImplemented();
}

void EmitBindlessImageSampleDrefImplicitLod(EmitContext&) {
    NotImplemented();
}

void EmitBindlessImageSampleDrefExplicitLod(EmitContext&) {
    NotImplemented();
}

void EmitBindlessImageGather(EmitContext&) {
    NotImplemented();
}

void EmitBindlessImageGatherDref(EmitContext&) {
    NotImplemented();
}

void EmitBindlessImageFetch(EmitContext&) {
    NotImplemented();
}

void EmitBindlessImageQueryDimensions(EmitContext&) {
    NotImplemented();
}

void EmitBindlessImageQueryLod(EmitContext&) {
    NotImplemented();
}

void EmitBindlessImageGradient(EmitContext&) {
    NotImplemented();
}

void EmitBindlessImageRead(EmitContext&) {
    NotImplemented();
}

void EmitBindlessImageWrite(EmitContext&) {
    NotImplemented();
}

void EmitBoundImageSampleImplicitLod(EmitContext&) {
    NotImplemented();
}

void EmitBoundImageSampleExplicitLod(EmitContext&) {
    NotImplemented();
}

void EmitBoundImageSampleDrefImplicitLod(EmitContext&) {
    NotImplemented();
}

void EmitBoundImageSampleDrefExplicitLod(EmitContext&) {
    NotImplemented();
}

void EmitBoundImageGather(EmitContext&) {
    NotImplemented();
}

void EmitBoundImageGatherDref(EmitContext&) {
    NotImplemented();
}

void EmitBoundImageFetch(EmitContext&) {
    NotImplemented();
}

void EmitBoundImageQueryDimensions(EmitContext&) {
    NotImplemented();
}

void EmitBoundImageQueryLod(EmitContext&) {
    NotImplemented();
}

void EmitBoundImageGradient(EmitContext&) {
    NotImplemented();
}

void EmitBoundImageRead(EmitContext&) {
    NotImplemented();
}

void EmitBoundImageWrite(EmitContext&) {
    NotImplemented();
}

} // namespace Shader::Backend::GLSL
