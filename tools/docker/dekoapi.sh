#!/bin/sh
H=/opt/devkitpro/libnx/include/deko3d.h
P=/opt/devkitpro/libnx/include/deko3d.hpp
echo "--- device flags"; grep -n -E 'DkDeviceFlags_' $H | head -14
echo "--- formats"; grep -n -E 'DkImageFormat_(R8_Unorm|RGBA8_Unorm|RGBA_BC1|RGBA_BC3|RGBA_BC1_Unorm|RGBA_BC3_Unorm|BC1|BC3)' $H | head
echo "--- enums"; grep -n -E 'DkBlendFactor_|DkBlendOp_Add|DkFilter_|DkWrapMode_(Clamp|ClampToEdge|Repeat)|DkPrimitive_Triangles|DkImageSwizzle_|DkVtxAttribSize_(2|4)x32|DkVtxAttribType_Float|DkImageType_2D|DkStage_(Vertex|Fragment)|DkStageFlag_GraphicsMask' $H | head -44
echo "--- constants"; grep -n -E 'define DK_(SHADER_CODE_ALIGNMENT|UNIFORM_BUF_ALIGNMENT|IMAGE_DESCRIPTOR_ALIGNMENT|SAMPLER_DESCRIPTOR_ALIGNMENT|MEMBLOCK_ALIGNMENT|CMDMEM_ALIGNMENT|SHADER_CODE_UNUSABLE_SIZE|IMAGE_LINEAR_STRIDE_ALIGNMENT)' $H
echo "--- hpp"; grep -n -E 'struct (VtxAttribState|VtxBufferState|RasterizerState|ColorState|ColorWriteState|DepthStencilState|BlendState|Scissor|Viewport|ImageView|Sampler|SamplerDescriptor|ImageDescriptor|ShaderMaker|ImageLayoutMaker|CopyBuf|ImageRect|BlendState)\b' $P | head -24
grep -n -E '(bindShaders|bindUniformBuffer|pushConstants|bindImageDescriptorSet|bindSamplerDescriptorSet|bindTextures|bindVtxAttribState|bindVtxBufferState|bindVtxBuffer|bindRasterizerState|bindColorState|bindColorWriteState|bindBlendStates|bindDepthStencilState|setViewports|setScissors|copyBufferToImage|draw\(|barrier|setVtxBufferStride|DkResHandle|dkMakeTextureHandle)' $P | head -30
grep -n -E 'dkMakeTextureHandle|DkResHandle|dkMakeImageHandle' $H | head
echo "--- setters"; grep -n -E 'setBlendEnable|setSrcColorBlendFactor|setDstColorBlendFactor|setFactors|setOps|setCullMode|setWrapMode|setFilter|setSwizzle|setLayout|setDepthTestEnable|setBlendEnable|setMemBlock|setCodeMemory' $P | head -20
sed -n '/struct DkVtxAttribState/,/}/p;/struct DkSamplerDescriptor\b/,/}/p' $H | head -24
echo "--- ex"; ls /opt/devkitpro/examples/switch/graphics/deko3d 2>/dev/null | head
