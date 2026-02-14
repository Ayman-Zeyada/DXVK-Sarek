#include "d3d9_format_helpers.h"

#include <d3d9_convert_yuy2_uyvy.h>
#include <d3d9_convert_l6v5u5.h>
#include <d3d9_convert_x8l8v8u8.h>
#include <d3d9_convert_a2w10v10u10.h>
#include <d3d9_convert_w11v11u10.h>
#include <d3d9_convert_nv12.h>
#include <d3d9_convert_yv12.h>
#include <d3d9_convert_bc1.h>
#include <d3d9_convert_bc2.h>
#include <d3d9_convert_bc3.h>

namespace dxvk {

  D3D9FormatHelper::D3D9FormatHelper(const Rc<DxvkDevice>& device)
    : m_device(device), m_context(m_device->createContext()) {
    m_context->beginRecording(
      m_device->createCommandList());

    InitShaders();
  }


  void D3D9FormatHelper::Flush() {
    if (m_transferCommands != 0)
      FlushInternal();
  }


  void D3D9FormatHelper::ConvertFormat(
          D3D9_CONVERSION_FORMAT_INFO   conversionFormat,
    const Rc<DxvkImage>&                dstImage,
          VkImageSubresourceLayers      dstSubresource,
    const DxvkBufferSlice&              srcSlice,
          VkOffset3D                    dstOffset,
          VkExtent3D                    dstExtent) {
    switch (conversionFormat.FormatType) {
      case D3D9ConversionFormat_YUY2:
      case D3D9ConversionFormat_UYVY: {
        uint32_t specConstant = conversionFormat.FormatType == D3D9ConversionFormat_UYVY ? 1 : 0;
        ConvertGenericFormat(conversionFormat, dstImage, dstSubresource, srcSlice, VK_FORMAT_R32_UINT, specConstant, { 2u, 1u }, dstOffset, dstExtent);
        break;
      }

      case D3D9ConversionFormat_NV12:
        ConvertGenericFormat(conversionFormat, dstImage, dstSubresource, srcSlice, VK_FORMAT_R16_UINT, 0, { 2u, 1u }, dstOffset, dstExtent);
        break;

      case D3D9ConversionFormat_YV12:
        ConvertGenericFormat(conversionFormat, dstImage, dstSubresource, srcSlice, VK_FORMAT_R8_UINT, 0, { 1u, 1u }, dstOffset, dstExtent);
        break;

      case D3D9ConversionFormat_L6V5U5:
        ConvertGenericFormat(conversionFormat, dstImage, dstSubresource, srcSlice, VK_FORMAT_R16_UINT, 0, { 1u, 1u }, dstOffset, dstExtent);
        break;

      case D3D9ConversionFormat_X8L8V8U8:
        ConvertGenericFormat(conversionFormat, dstImage, dstSubresource, srcSlice, VK_FORMAT_R32_UINT, 0, { 1u, 1u }, dstOffset, dstExtent);
        break;

      case D3D9ConversionFormat_A2W10V10U10:
        ConvertGenericFormat(conversionFormat, dstImage, dstSubresource, srcSlice, VK_FORMAT_R32_UINT, 0, { 1u, 1u }, dstOffset, dstExtent);
        break;

      case D3D9ConversionFormat_W11V11U10:
        ConvertGenericFormat(conversionFormat, dstImage, dstSubresource, srcSlice, VK_FORMAT_R32_UINT, 0, { 1u, 1u }, dstOffset, dstExtent);
        break;

      case D3D9ConversionFormat_BC1:
      case D3D9ConversionFormat_BC2:
      case D3D9ConversionFormat_BC3:
        ConvertGenericFormat(conversionFormat, dstImage, dstSubresource, srcSlice, VK_FORMAT_R32_UINT, 0, { 4u, 4u }, dstOffset, dstExtent);
        break;

      default:
        Logger::warn("Unimplemented format conversion");
    }
  }


  void D3D9FormatHelper::ConvertGenericFormat(
          D3D9_CONVERSION_FORMAT_INFO   videoFormat,
    const Rc<DxvkImage>&                dstImage,
          VkImageSubresourceLayers      dstSubresource,
    const DxvkBufferSlice&              srcSlice,
          VkFormat                      bufferFormat,
          uint32_t                      specConstantValue,
          VkExtent2D                    macroPixelRun,
          VkOffset3D                    dstOffset,
          VkExtent3D                    dstExtent) {
    DxvkImageViewCreateInfo imageViewInfo;
    imageViewInfo.type      = VK_IMAGE_VIEW_TYPE_2D;
    imageViewInfo.format    = dstImage->info().format;
    imageViewInfo.usage     = VK_IMAGE_USAGE_STORAGE_BIT;
    imageViewInfo.aspect    = dstSubresource.aspectMask;
    imageViewInfo.minLevel  = dstSubresource.mipLevel;
    imageViewInfo.numLevels = 1;
    imageViewInfo.minLayer  = dstSubresource.baseArrayLayer;
    imageViewInfo.numLayers = dstSubresource.layerCount;
    auto tmpImageView = m_device->createImageView(dstImage, imageViewInfo);

    VkExtent3D macroExtent = {
      (dstExtent.width + macroPixelRun.width - 1) / macroPixelRun.width,
      (dstExtent.height + macroPixelRun.height - 1) / macroPixelRun.height,
      1u };

    DxvkBufferViewCreateInfo bufferViewInfo;
    bufferViewInfo.format      = bufferFormat;
    bufferViewInfo.rangeOffset = srcSlice.offset();
    bufferViewInfo.rangeLength = srcSlice.length();
    auto tmpBufferView = m_device->createBufferView(srcSlice.buffer(), bufferViewInfo);

    if (specConstantValue)
      m_context->setSpecConstant(VK_PIPELINE_BIND_POINT_COMPUTE, 0, specConstantValue);

    D3D9ConvertPushConstants pushArgs;
    pushArgs.extent = { macroExtent.width, macroExtent.height };
    pushArgs.dstOffset = { dstOffset.x, dstOffset.y };

    m_context->bindResourceView(BindingIds::Image,  tmpImageView, nullptr);
    m_context->bindResourceView(BindingIds::Buffer, nullptr,     tmpBufferView);
    m_context->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, m_shaders[videoFormat.FormatType]);
    m_context->pushConstants(0, sizeof(pushArgs), &pushArgs);
    m_context->dispatch(
      (macroExtent.width  / 8) + (macroExtent.width  % 8),
      (macroExtent.height / 8) + (macroExtent.height % 8),
      1);

    // Reset the spec constants used...
    if (specConstantValue)
      m_context->setSpecConstant(VK_PIPELINE_BIND_POINT_COMPUTE, 0, 0);
    
    m_transferCommands += 1;
  }


  void D3D9FormatHelper::InitShaders() {
    m_shaders[D3D9ConversionFormat_YUY2] = InitShader(d3d9_convert_yuy2_uyvy);
    m_shaders[D3D9ConversionFormat_UYVY] = m_shaders[D3D9ConversionFormat_YUY2];
    m_shaders[D3D9ConversionFormat_L6V5U5] = InitShader(d3d9_convert_l6v5u5);
    m_shaders[D3D9ConversionFormat_X8L8V8U8] = InitShader(d3d9_convert_x8l8v8u8);
    m_shaders[D3D9ConversionFormat_A2W10V10U10] = InitShader(d3d9_convert_a2w10v10u10);
    m_shaders[D3D9ConversionFormat_W11V11U10] = InitShader(d3d9_convert_w11v11u10);
    m_shaders[D3D9ConversionFormat_NV12] = InitShader(d3d9_convert_nv12);
    m_shaders[D3D9ConversionFormat_YV12] = InitShader(d3d9_convert_yv12);
    m_shaders[D3D9ConversionFormat_BC1] = InitShader(d3d9_convert_bc1);
    m_shaders[D3D9ConversionFormat_BC2] = InitShader(d3d9_convert_bc2);
    m_shaders[D3D9ConversionFormat_BC3] = InitShader(d3d9_convert_bc3);
  }


  Rc<DxvkShader> D3D9FormatHelper::InitShader(SpirvCodeBuffer code) {
    const std::array<DxvkResourceSlot, 2> resourceSlots = { {
      { BindingIds::Image,  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,        VK_IMAGE_VIEW_TYPE_2D },
      { BindingIds::Buffer, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_IMAGE_VIEW_TYPE_1D },
    } };

    DxvkShaderCreateInfo info;
    info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.resourceSlotCount = resourceSlots.size();
    info.resourceSlots = resourceSlots.data();
    info.pushConstOffset = 0;
    info.pushConstSize = sizeof(D3D9ConvertPushConstants);

    return new DxvkShader(info, std::move(code));
  }


  void D3D9FormatHelper::FlushInternal() {
    m_context->flushCommandList();
    
    m_transferCommands = 0;
  }

}
