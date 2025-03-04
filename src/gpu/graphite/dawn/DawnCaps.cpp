/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/graphite/dawn/DawnCaps.h"

#include <algorithm>

#include "include/core/SkTextureCompressionType.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/TextureInfo.h"
#include "include/gpu/graphite/dawn/DawnBackendContext.h"
#include "src/gpu/graphite/ComputePipelineDesc.h"
#include "src/gpu/graphite/GraphicsPipelineDesc.h"
#include "src/gpu/graphite/GraphiteResourceKey.h"
#include "src/gpu/graphite/RenderPassDesc.h"
#include "src/gpu/graphite/ResourceTypes.h"
#include "src/gpu/graphite/UniformManager.h"
#include "src/gpu/graphite/dawn/DawnGraphiteUtilsPriv.h"
#include "src/gpu/graphite/dawn/DawnUtilsPriv.h"
#include "src/sksl/SkSLUtil.h"

namespace {

// These are all the valid wgpu::TextureFormat that we currently support in Skia.
// They are roughly ordered from most frequently used to least to improve lookup times in arrays.
static constexpr wgpu::TextureFormat kFormats[skgpu::graphite::DawnCaps::kFormatCnt] = {
        wgpu::TextureFormat::RGBA8Unorm,
        wgpu::TextureFormat::R8Unorm,
#if !defined(__EMSCRIPTEN__)
        wgpu::TextureFormat::R16Unorm,
#endif
        wgpu::TextureFormat::BGRA8Unorm,
        wgpu::TextureFormat::RGBA16Float,
        wgpu::TextureFormat::R16Float,
        wgpu::TextureFormat::RG8Unorm,
#if !defined(__EMSCRIPTEN__)
        wgpu::TextureFormat::RG16Unorm,
#endif
        wgpu::TextureFormat::RGB10A2Unorm,
        wgpu::TextureFormat::RG16Float,

        wgpu::TextureFormat::Stencil8,
        wgpu::TextureFormat::Depth16Unorm,
        wgpu::TextureFormat::Depth32Float,
        wgpu::TextureFormat::Depth24PlusStencil8,

        wgpu::TextureFormat::BC1RGBAUnorm,
        wgpu::TextureFormat::ETC2RGB8Unorm,

#if !defined(__EMSCRIPTEN__)
        wgpu::TextureFormat::External,
#endif

        wgpu::TextureFormat::Undefined,
};

#if !defined(__EMSCRIPTEN__)
bool IsMultiplanarFormat(wgpu::TextureFormat format) {
    switch (format) {
        case wgpu::TextureFormat::R8BG8Biplanar420Unorm:
        case wgpu::TextureFormat::R10X6BG10X6Biplanar420Unorm:
        case wgpu::TextureFormat::R8BG8A8Triplanar420Unorm:
            return true;
        default:
            return false;
    }
}
#endif
}  // anonymous namespace

namespace skgpu::graphite {

DawnCaps::DawnCaps(const DawnBackendContext& backendContext, const ContextOptions& options)
    : Caps() {
    this->initCaps(backendContext, options);
    this->initShaderCaps(backendContext.fDevice);
    this->initFormatTable(backendContext.fDevice);
    this->finishInitialization(options);
}

DawnCaps::~DawnCaps() = default;

uint32_t DawnCaps::channelMask(const TextureInfo& info) const {
    return DawnFormatChannels(info.dawnTextureSpec().getViewFormat());
}

bool DawnCaps::onIsTexturable(const TextureInfo& info) const {
    if (!info.isValid()) {
        return false;
    }

    const auto& spec = info.dawnTextureSpec();

    if (!(spec.fUsage & wgpu::TextureUsage::TextureBinding)) {
        return false;
    }

#if !defined(__EMSCRIPTEN__)
    switch (spec.fFormat) {
        case wgpu::TextureFormat::R8BG8Biplanar420Unorm: {
            if (spec.fAspect == wgpu::TextureAspect::Plane0Only &&
                spec.getViewFormat() != wgpu::TextureFormat::R8Unorm) {
                return false;
            }
            if (spec.fAspect == wgpu::TextureAspect::Plane1Only &&
                spec.getViewFormat() != wgpu::TextureFormat::RG8Unorm) {
                return false;
            }
            break;
        }
        case wgpu::TextureFormat::R10X6BG10X6Biplanar420Unorm: {
            if (spec.fAspect == wgpu::TextureAspect::Plane0Only &&
                spec.getViewFormat() != wgpu::TextureFormat::R16Unorm) {
                return false;
            }
            if (spec.fAspect == wgpu::TextureAspect::Plane1Only &&
                spec.getViewFormat() != wgpu::TextureFormat::RG16Unorm) {
                return false;
            }
            break;
        }
        case wgpu::TextureFormat::R8BG8A8Triplanar420Unorm: {
            if (spec.fAspect == wgpu::TextureAspect::Plane0Only &&
                spec.getViewFormat() != wgpu::TextureFormat::R8Unorm) {
                return false;
            }
            if (spec.fAspect == wgpu::TextureAspect::Plane1Only &&
                spec.getViewFormat() != wgpu::TextureFormat::RG8Unorm) {
                return false;
            }
            if (spec.fAspect == wgpu::TextureAspect::Plane2Only &&
                spec.getViewFormat() != wgpu::TextureFormat::R8Unorm) {
                return false;
            }
            break;
        }
        default:
            break;
    }
#endif

    return this->isTexturable(info.dawnTextureSpec().getViewFormat());
}

bool DawnCaps::isTexturable(wgpu::TextureFormat format) const {
    const FormatInfo& formatInfo = this->getFormatInfo(format);
    return SkToBool(FormatInfo::kTexturable_Flag & formatInfo.fFlags);
}

bool DawnCaps::isRenderable(const TextureInfo& info) const {
    return info.isValid() &&
           (info.dawnTextureSpec().fUsage & wgpu::TextureUsage::RenderAttachment) &&
           this->isRenderable(info.dawnTextureSpec().getViewFormat(), info.numSamples());
}

bool DawnCaps::isStorage(const TextureInfo& info) const {
    if (!info.isValid()) {
        return false;
    }
    if (!(info.dawnTextureSpec().fUsage & wgpu::TextureUsage::StorageBinding)) {
        return false;
    }
    const FormatInfo& formatInfo = this->getFormatInfo(info.dawnTextureSpec().getViewFormat());
    return info.numSamples() == 1 && SkToBool(FormatInfo::kStorage_Flag & formatInfo.fFlags);
}

uint32_t DawnCaps::maxRenderTargetSampleCount(wgpu::TextureFormat format) const {
    const FormatInfo& formatInfo = this->getFormatInfo(format);
    if (!SkToBool(formatInfo.fFlags & FormatInfo::kRenderable_Flag)) {
        return 0;
    }
    if (SkToBool(formatInfo.fFlags & FormatInfo::kMSAA_Flag)) {
        return 8;
    } else {
        return 1;
    }
}

bool DawnCaps::isRenderable(wgpu::TextureFormat format, uint32_t sampleCount) const {
    return sampleCount <= this->maxRenderTargetSampleCount(format);
}

TextureInfo DawnCaps::getDefaultSampledTextureInfo(SkColorType colorType,
                                                   Mipmapped mipmapped,
                                                   Protected,
                                                   Renderable renderable) const {
    wgpu::TextureUsage usage = wgpu::TextureUsage::TextureBinding |
                               wgpu::TextureUsage::CopyDst |
                               wgpu::TextureUsage::CopySrc;
    if (renderable == Renderable::kYes) {
        usage |= wgpu::TextureUsage::RenderAttachment;
    }

    wgpu::TextureFormat format = this->getFormatFromColorType(colorType);
    if (format == wgpu::TextureFormat::Undefined) {
        return {};
    }

    DawnTextureInfo info;
    info.fSampleCount = 1;
    info.fMipmapped = mipmapped;
    info.fFormat = format;
    info.fViewFormat = format;
    info.fUsage = usage;

    return info;
}

TextureInfo DawnCaps::getTextureInfoForSampledCopy(const TextureInfo& textureInfo,
                                                   Mipmapped mipmapped) const {
    DawnTextureInfo info;
    if (!textureInfo.getDawnTextureInfo(&info)) {
        return {};
    }

    info.fSampleCount = 1;
    info.fMipmapped = mipmapped;
    info.fUsage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst |
                  wgpu::TextureUsage::CopySrc;

    return info;
}

namespace {
wgpu::TextureFormat format_from_compression(SkTextureCompressionType compression) {
    switch (compression) {
        case SkTextureCompressionType::kETC2_RGB8_UNORM:
            return wgpu::TextureFormat::ETC2RGB8Unorm;
        case SkTextureCompressionType::kBC1_RGBA8_UNORM:
            return wgpu::TextureFormat::BC1RGBAUnorm;
        default:
            return wgpu::TextureFormat::Undefined;
    }
}
}

TextureInfo DawnCaps::getDefaultCompressedTextureInfo(SkTextureCompressionType compression,
                                                      Mipmapped mipmapped,
                                                      Protected) const {
    wgpu::TextureUsage usage = wgpu::TextureUsage::TextureBinding |
                               wgpu::TextureUsage::CopyDst |
                               wgpu::TextureUsage::CopySrc;

    wgpu::TextureFormat format = format_from_compression(compression);
    if (format == wgpu::TextureFormat::Undefined) {
        return {};
    }

    DawnTextureInfo info;
    info.fSampleCount = 1;
    info.fMipmapped = mipmapped;
    info.fFormat = format;
    info.fViewFormat = format;
    info.fUsage = usage;

    return info;
}

TextureInfo DawnCaps::getDefaultMSAATextureInfo(const TextureInfo& singleSampledInfo,
                                                Discardable discardable) const {
    if (fDefaultMSAASamples <= 1) {
        return {};
    }
    const DawnTextureSpec& singleSpec = singleSampledInfo.dawnTextureSpec();

    DawnTextureInfo info;
    info.fSampleCount = fDefaultMSAASamples;
    info.fMipmapped   = Mipmapped::kNo;
    info.fFormat      = singleSpec.fFormat;
    info.fViewFormat = singleSpec.fFormat;
    info.fUsage       = wgpu::TextureUsage::RenderAttachment;

    if (fSupportedTransientAttachmentUsage != wgpu::TextureUsage::None &&
        discardable == Discardable::kYes) {
        info.fUsage |= fSupportedTransientAttachmentUsage;
    }

    return info;
}

TextureInfo DawnCaps::getDefaultDepthStencilTextureInfo(
    SkEnumBitMask<DepthStencilFlags> depthStencilType,
    uint32_t sampleCount,
    Protected) const {
    DawnTextureInfo info;
    info.fSampleCount = sampleCount;
    info.fMipmapped   = Mipmapped::kNo;
    info.fFormat      = DawnDepthStencilFlagsToFormat(depthStencilType);
    info.fViewFormat = info.fFormat;
    info.fUsage       = wgpu::TextureUsage::RenderAttachment;

    if (fSupportedTransientAttachmentUsage != wgpu::TextureUsage::None) {
        info.fUsage |= fSupportedTransientAttachmentUsage;
    }

    return info;
}

TextureInfo DawnCaps::getDefaultStorageTextureInfo(SkColorType colorType) const {
    wgpu::TextureFormat format = this->getFormatFromColorType(colorType);
    if (format == wgpu::TextureFormat::Undefined) {
        SkDebugf("colorType=%d is not supported\n", static_cast<int>(colorType));
        return {};
    }

    const FormatInfo& formatInfo = this->getFormatInfo(format);
    if (!SkToBool(FormatInfo::kStorage_Flag & formatInfo.fFlags)) {
        return {};
    }

    wgpu::TextureUsage usage = wgpu::TextureUsage::StorageBinding |
                               wgpu::TextureUsage::TextureBinding |
                               wgpu::TextureUsage::CopySrc;
    DawnTextureInfo info;
    info.fSampleCount = 1;
    info.fMipmapped = Mipmapped::kNo;
    info.fFormat = format;
    info.fViewFormat = format;
    info.fUsage = usage;

    return info;
}

SkISize DawnCaps::getDepthAttachmentDimensions(const TextureInfo& textureInfo,
                                               const SkISize colorAttachmentDimensions) const {
#if !defined(__EMSCRIPTEN__)
    // For multiplanar textures, texture->textureInfo() uses the format of planes instead of
    // textures (R8, R8G8, vs R8BG8Biplanar420Unorm), so we have to query texture format from
    // wgpu::Texture object, and then use it reconstruct the full dimensions.
    const auto& dawnTextureSpec = textureInfo.dawnTextureSpec();
    wgpu::TextureFormat format = dawnTextureSpec.fFormat;
    if (IsMultiplanarFormat(format) && dawnTextureSpec.fAspect == wgpu::TextureAspect::Plane1Only) {
        // Dawn requires depth attachment to match the size of Y plane (texture size).
        return SkISize::Make(colorAttachmentDimensions.width() * 2,
                             colorAttachmentDimensions.height() * 2);
    }
#endif

    return colorAttachmentDimensions;
}

const Caps::ColorTypeInfo* DawnCaps::getColorTypeInfo(SkColorType colorType,
                                                      const TextureInfo& textureInfo) const {
    auto dawnFormat = textureInfo.dawnTextureSpec().getViewFormat();
    if (dawnFormat == wgpu::TextureFormat::Undefined) {
        SkASSERT(false);
        return nullptr;
    }

    const FormatInfo& info = this->getFormatInfo(dawnFormat);
    for (int i = 0; i < info.fColorTypeInfoCount; ++i) {
        const ColorTypeInfo& ctInfo = info.fColorTypeInfos[i];
        if (ctInfo.fColorType == colorType) {
            return &ctInfo;
        }
    }

    return nullptr;
}

bool DawnCaps::supportsWritePixels(const TextureInfo& textureInfo) const {
    const auto& spec = textureInfo.dawnTextureSpec();
    return spec.fUsage & wgpu::TextureUsage::CopyDst;
}

bool DawnCaps::supportsReadPixels(const TextureInfo& textureInfo) const {
    const auto& spec = textureInfo.dawnTextureSpec();
    return spec.fUsage & wgpu::TextureUsage::CopySrc;
}

std::pair<SkColorType, bool /*isRGBFormat*/> DawnCaps::supportedWritePixelsColorType(
        SkColorType dstColorType,
        const TextureInfo& dstTextureInfo,
        SkColorType srcColorType) const {
    return {dstColorType, false};
}

std::pair<SkColorType, bool /*isRGBFormat*/> DawnCaps::supportedReadPixelsColorType(
        SkColorType srcColorType,
        const TextureInfo& srcTextureInfo,
        SkColorType dstColorType) const {
    auto dawnFormat = getFormatFromColorType(srcColorType);
    const FormatInfo& info = this->getFormatInfo(dawnFormat);
    for (int i = 0; i < info.fColorTypeInfoCount; ++i) {
        const auto& ctInfo = info.fColorTypeInfos[i];
        if (ctInfo.fColorType == srcColorType) {
            return {srcColorType, false};
        }
    }
    return {kUnknown_SkColorType, false};
}

void DawnCaps::initCaps(const DawnBackendContext& backendContext, const ContextOptions& options) {
    // GetAdapter() is not available in WASM and there's no way to get AdapterProperties off of
    // the WGPUDevice directly.
#if !defined(__EMSCRIPTEN__)
    wgpu::AdapterProperties props;
    backendContext.fDevice.GetAdapter().GetProperties(&props);

#if defined(GRAPHITE_TEST_UTILS)
    this->setDeviceName(props.name);
#endif
#endif // defined(__EMSCRIPTEN__)

    wgpu::SupportedLimits limits;

    [[maybe_unused]] bool limitsSucceeded = backendContext.fDevice.GetLimits(&limits);
    // In Emscripten this always "fails" until
    // https://github.com/emscripten-core/emscripten/pull/20808, which was first included in 3.1.51.
#if !defined(__EMSCRIPTEN__)                                     || \
        (__EMSCRIPTEN_major__ >  3                               || \
        (__EMSCRIPTEN_major__ == 3 && __EMSCRIPTEN_minor__ >  1) || \
        (__EMSCRIPTEN_major__ == 3 && __EMSCRIPTEN_minor__ == 1 && __EMSCRIPTEN_tiny__ > 50))
    SkASSERT(limitsSucceeded);
#endif

    fMaxTextureSize = limits.limits.maxTextureDimension2D;

    fRequiredTransferBufferAlignment = 4;
    fRequiredUniformBufferAlignment = 256;
    fRequiredStorageBufferAlignment = fRequiredUniformBufferAlignment;

    // Dawn requires 256 bytes per row alignment for buffer texture copies.
    fTextureDataRowBytesAlignment = 256;

    fResourceBindingReqs.fUniformBufferLayout = Layout::kStd140;
    // The WGSL generator assumes tightly packed std430 layout for SSBOs which is also the default
    // for all types outside the uniform address space in WGSL.
    fResourceBindingReqs.fStorageBufferLayout = Layout::kStd430;
    fResourceBindingReqs.fSeparateTextureAndSamplerBinding = true;

#if !defined(__EMSCRIPTEN__)
    // TODO(b/318817249): SSBOs trigger FXC compiler failures when attempting to unroll loops
    fStorageBufferSupport = props.backendType != wgpu::BackendType::D3D11;
    fStorageBufferPreferred = props.backendType != wgpu::BackendType::D3D11;
#else
    // WASM doesn't provide a way to query the backend, so can't tell if we are on d3d11 or not.
    // Pessimistically assume we could be. Once b/318817249 is fixed, this can go away and SSBOs
    // can always be enabled.
    fStorageBufferSupport = false;
    fStorageBufferPreferred = false;
#endif

    fDrawBufferCanBeMapped = false;

    fComputeSupport = true;

    // TODO: support clamp to border.
    fClampToBorderSupport = false;

#if defined(GRAPHITE_TEST_UTILS)
    fDrawBufferCanBeMappedForReadback = false;
#endif

#if defined(__EMSCRIPTEN__)
    // For wasm, we use async map.
    fBufferMapsAreAsync = true;
#else
    // For Dawn native, we use direct mapping.
    fBufferMapsAreAsync = false;
    fDrawBufferCanBeMapped =
            backendContext.fDevice.HasFeature(wgpu::FeatureName::BufferMapExtendedUsages);

    fMSAARenderToSingleSampledSupport =
            backendContext.fDevice.HasFeature(wgpu::FeatureName::MSAARenderToSingleSampled);

    if (backendContext.fDevice.HasFeature(wgpu::FeatureName::TransientAttachments)) {
        fSupportedTransientAttachmentUsage = wgpu::TextureUsage::TransientAttachment;
    }
    if (backendContext.fDevice.HasFeature(wgpu::FeatureName::DawnLoadResolveTexture)) {
        fSupportedResolveTextureLoadOp = wgpu::LoadOp::ExpandResolveTexture;
    }
#endif

    if (!backendContext.fTick) {
        fAllowCpuSync = false;
        // This seems paradoxical. However, if we use the async pipeline creation methods (e.g
        // Device::CreateRenderPipelineAsync) then we may have to synchronize before a submit that
        // uses the pipeline. If we use the methods that look synchronous (e.g.
        // Device::CreateRenderPipeline) they actually operate asynchronously on WebGPU but the
        // browser becomes responsible for synchronizing when we call submit.
        fUseAsyncPipelineCreation = false;

        // The implementation busy waits after popping.
        fAllowScopedErrorChecks = false;
    }

    fFullCompressedUploadSizeMustAlignToBlockDims = true;
}

void DawnCaps::initShaderCaps(const wgpu::Device& device) {
    SkSL::ShaderCaps* shaderCaps = fShaderCaps.get();

    // WGSL does not support infinities regardless of hardware support. There are discussions around
    // enabling it using an extension in the future.
    shaderCaps->fInfinitySupport = false;

    // WGSL supports shader derivatives in the fragment shader
    shaderCaps->fShaderDerivativeSupport = true;

#if !defined(__EMSCRIPTEN__)
    if (device.HasFeature(wgpu::FeatureName::DualSourceBlending)) {
        shaderCaps->fDualSourceBlendingSupport = true;
    }
    if (device.HasFeature(wgpu::FeatureName::FramebufferFetch)) {
        shaderCaps->fFBFetchSupport = true;
    }
#endif
}

void DawnCaps::initFormatTable(const wgpu::Device& device) {
    FormatInfo* info;
    // Format: RGBA8Unorm
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::RGBA8Unorm)];
        info->fFlags = FormatInfo::kAllFlags;
        info->fColorTypeInfoCount = 2;
        info->fColorTypeInfos = std::make_unique<ColorTypeInfo[]>(info->fColorTypeInfoCount);
        int ctIdx = 0;
        // Format: RGBA8Unorm, Surface: kRGBA_8888
        {
            auto& ctInfo = info->fColorTypeInfos[ctIdx++];
            ctInfo.fColorType = kRGBA_8888_SkColorType;
            ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag | ColorTypeInfo::kRenderable_Flag;
        }
        // Format: RGBA8Unorm, Surface: kRGB_888x
        {
            auto& ctInfo = info->fColorTypeInfos[ctIdx++];
            ctInfo.fColorType = kRGB_888x_SkColorType;
            ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag;
            ctInfo.fReadSwizzle = skgpu::Swizzle::RGB1();
        }
    }

    // Format: R8Unorm
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::R8Unorm)];
#if !defined(__EMSCRIPTEN__)
        info->fFlags = FormatInfo::kAllFlags;
        if (!device.HasFeature(wgpu::FeatureName::R8UnormStorage)) {
            info->fFlags &= ~FormatInfo::kStorage_Flag;
        }
#else
        info->fFlags = FormatInfo::kAllFlags & ~FormatInfo::kStorage_Flag;
#endif
        info->fColorTypeInfoCount = 3;
        info->fColorTypeInfos = std::make_unique<ColorTypeInfo[]>(info->fColorTypeInfoCount);
        int ctIdx = 0;
        // Format: R8Unorm, Surface: kR8_unorm
        {
            auto& ctInfo = info->fColorTypeInfos[ctIdx++];
            ctInfo.fColorType = kR8_unorm_SkColorType;
            ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag | ColorTypeInfo::kRenderable_Flag;
        }
        // Format: R8Unorm, Surface: kAlpha_8
        {
            auto& ctInfo = info->fColorTypeInfos[ctIdx++];
            ctInfo.fColorType = kAlpha_8_SkColorType;
            ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag | ColorTypeInfo::kRenderable_Flag;
            ctInfo.fReadSwizzle = skgpu::Swizzle("000r");
            ctInfo.fWriteSwizzle = skgpu::Swizzle("a000");
        }
        // Format: R8Unorm, Surface: kGray_8
        {
            auto& ctInfo = info->fColorTypeInfos[ctIdx++];
            ctInfo.fColorType = kGray_8_SkColorType;
            ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag;
            ctInfo.fReadSwizzle = skgpu::Swizzle("rrr1");
        }
    }

#if !defined(__EMSCRIPTEN__)
    const bool supportUnorm16 = device.HasFeature(wgpu::FeatureName::Unorm16TextureFormats);
    // TODO(crbug.com/dawn/1856): Support storage binding for compute shader in Dawn.
    // Format: R16Unorm
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::R16Unorm)];
        if (supportUnorm16) {
            info->fFlags = FormatInfo::kAllFlags & ~FormatInfo::kStorage_Flag;
            info->fColorTypeInfoCount = 1;
            info->fColorTypeInfos = std::make_unique<ColorTypeInfo[]>(info->fColorTypeInfoCount);
            int ctIdx = 0;
            // Format: R16Unorm, Surface: kA16_unorm
            {
                auto& ctInfo = info->fColorTypeInfos[ctIdx++];
                ctInfo.fColorType = kA16_unorm_SkColorType;
                ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag | ColorTypeInfo::kRenderable_Flag;
                ctInfo.fReadSwizzle = skgpu::Swizzle("000r");
                ctInfo.fWriteSwizzle = skgpu::Swizzle("a000");
            }
        }
    }
#endif

    // Format: BGRA8Unorm
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::BGRA8Unorm)];
        info->fFlags = FormatInfo::kAllFlags;
        info->fColorTypeInfoCount = 2;
        info->fColorTypeInfos = std::make_unique<ColorTypeInfo[]>(info->fColorTypeInfoCount);
        int ctIdx = 0;
        // Format: BGRA8Unorm, Surface: kBGRA_8888
        {
            auto& ctInfo = info->fColorTypeInfos[ctIdx++];
            ctInfo.fColorType = kBGRA_8888_SkColorType;
            ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag | ColorTypeInfo::kRenderable_Flag;
        }
        // Format: BGRA8Unorm, Surface: kRGB_888x
        {
            auto& ctInfo = info->fColorTypeInfos[ctIdx++];
            ctInfo.fColorType = kRGB_888x_SkColorType;
            ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag;
        }
    }

    // Format: RGBA16Float
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::RGBA16Float)];
        info->fFlags = FormatInfo::kAllFlags;
        info->fColorTypeInfoCount = 1;
        info->fColorTypeInfos = std::make_unique<ColorTypeInfo[]>(info->fColorTypeInfoCount);
        int ctIdx = 0;
        // Format: RGBA16Float, Surface: RGBA_F16
        {
            auto& ctInfo = info->fColorTypeInfos[ctIdx++];
            ctInfo.fColorType = kRGBA_F16_SkColorType;
            ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag | ColorTypeInfo::kRenderable_Flag;
        }
    }

    // Format: R16Float
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::R16Float)];
        info->fFlags = FormatInfo::kAllFlags;
        info->fColorTypeInfoCount = 1;
        info->fColorTypeInfos = std::make_unique<ColorTypeInfo[]>(info->fColorTypeInfoCount);
        int ctIdx = 0;
        // Format: R16Float, Surface: kA16_float
        {
            auto& ctInfo = info->fColorTypeInfos[ctIdx++];
            ctInfo.fColorType = kA16_float_SkColorType;
            ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag | ColorTypeInfo::kRenderable_Flag;
            ctInfo.fReadSwizzle = skgpu::Swizzle("000r");
            ctInfo.fWriteSwizzle = skgpu::Swizzle("a000");
        }
    }

    // TODO(crbug.com/dawn/1856): Support storage binding for compute shader in Dawn.
    // Format: RG8Unorm
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::RG8Unorm)];
        info->fFlags = FormatInfo::kAllFlags;
        info->fColorTypeInfoCount = 1;
        info->fColorTypeInfos = std::make_unique<ColorTypeInfo[]>(info->fColorTypeInfoCount);
        int ctIdx = 0;
        // Format: RG8Unorm, Surface: kR8G8_unorm
        {
            auto& ctInfo = info->fColorTypeInfos[ctIdx++];
            ctInfo.fColorType = kR8G8_unorm_SkColorType;
            ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag | ColorTypeInfo::kRenderable_Flag;
        }
    }

#if !defined(__EMSCRIPTEN__)
    // TODO(crbug.com/dawn/1856): Support storage binding for compute shader in Dawn.
    // Format: RG16Unorm
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::RG16Unorm)];
        if (supportUnorm16) {
            info->fFlags = FormatInfo::kAllFlags;
            info->fColorTypeInfoCount = 1;
            info->fColorTypeInfos = std::make_unique<ColorTypeInfo[]>(info->fColorTypeInfoCount);
            int ctIdx = 0;
            // Format: RG16Unorm, Surface: kR16G16_unorm
            {
                auto& ctInfo = info->fColorTypeInfos[ctIdx++];
                ctInfo.fColorType = kR16G16_unorm_SkColorType;
                ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag | ColorTypeInfo::kRenderable_Flag;
            }
        }
    }
#endif

    // Format: RGB10A2Unorm
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::RGB10A2Unorm)];
        info->fFlags = FormatInfo::kAllFlags;
        info->fColorTypeInfoCount = 1;
        info->fColorTypeInfos = std::make_unique<ColorTypeInfo[]>(info->fColorTypeInfoCount);
        int ctIdx = 0;
        // Format: RGB10A2Unorm, Surface: kRGBA_1010102
        {
            auto& ctInfo = info->fColorTypeInfos[ctIdx++];
            ctInfo.fColorType = kRGBA_1010102_SkColorType;
            ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag | ColorTypeInfo::kRenderable_Flag;
        }
    }

    // Format: RG16Float
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::RG16Float)];
        info->fFlags = FormatInfo::kAllFlags;
        info->fColorTypeInfoCount = 1;
        info->fColorTypeInfos = std::make_unique<ColorTypeInfo[]>(info->fColorTypeInfoCount);
        int ctIdx = 0;
        // Format: RG16Float, Surface: kR16G16_float
        {
            auto& ctInfo = info->fColorTypeInfos[ctIdx++];
            ctInfo.fColorType = kR16G16_float_SkColorType;
            ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag | ColorTypeInfo::kRenderable_Flag;
        }
    }

    // Format: ETC2RGB8Unorm
    {
        if (device.HasFeature(wgpu::FeatureName::TextureCompressionETC2)) {
            info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::ETC2RGB8Unorm)];
            info->fFlags = FormatInfo::kTexturable_Flag;
            info->fColorTypeInfoCount = 1;
            info->fColorTypeInfos = std::make_unique<ColorTypeInfo[]>(info->fColorTypeInfoCount);
            int ctIdx = 0;
            // Format: ETC2RGB8Unorm, Surface: kRGB_888x
            {
                auto& ctInfo = info->fColorTypeInfos[ctIdx++];
                ctInfo.fColorType = kRGB_888x_SkColorType;
                ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag;
            }
        }
    }

    // Format: BC1RGBAUnorm
    {
        if (device.HasFeature(wgpu::FeatureName::TextureCompressionBC)) {
            info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::BC1RGBAUnorm)];
            info->fFlags = FormatInfo::kTexturable_Flag;
            info->fColorTypeInfoCount = 1;
            info->fColorTypeInfos = std::make_unique<ColorTypeInfo[]>(info->fColorTypeInfoCount);
            int ctIdx = 0;
            // Format: BC1RGBAUnorm, Surface: kRGBA_8888
            {
                auto& ctInfo = info->fColorTypeInfos[ctIdx++];
                ctInfo.fColorType = kRGBA_8888_SkColorType;
                ctInfo.fFlags = ColorTypeInfo::kUploadData_Flag;
            }
        }
    }

    /*
     * Non-color formats
     */

    // Format: Stencil8
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::Stencil8)];
        info->fFlags = FormatInfo::kMSAA_Flag;
        info->fColorTypeInfoCount = 0;
    }

    // Format: Depth16UNorm
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::Depth16Unorm)];
        info->fFlags = FormatInfo::kMSAA_Flag;
        info->fColorTypeInfoCount = 0;
    }

    // Format: Depth32Float
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::Depth32Float)];
        info->fFlags = FormatInfo::kMSAA_Flag;
        info->fColorTypeInfoCount = 0;
    }

    // Format: Depth24PlusStencil8
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::Depth24PlusStencil8)];
        info->fFlags = FormatInfo::kMSAA_Flag;
        info->fColorTypeInfoCount = 0;
    }

#if !defined(__EMSCRIPTEN__)
    // Format: External
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::External)];
        info->fFlags = FormatInfo::kTexturable_Flag;
        info->fColorTypeInfoCount = 1;
        info->fColorTypeInfos = std::make_unique<ColorTypeInfo[]>(info->fColorTypeInfoCount);
        int ctIdx = 0;
        // Format: External, Surface: kRGBA_8888
        {
            auto& ctInfo = info->fColorTypeInfos[ctIdx++];
            ctInfo.fColorType = kRGBA_8888_SkColorType;
        }
    }
#endif

    // Format: Undefined
    {
        info = &fFormatTable[GetFormatIndex(wgpu::TextureFormat::Undefined)];
        info->fFlags = 0;
        info->fColorTypeInfoCount = 0;
    }

    ////////////////////////////////////////////////////////////////////////////
    // Map SkColorTypes (used for creating SkSurfaces) to wgpu::TextureFormat.
    // The order in which the formats are passed into the setColorType function
    // indicates the priority in selecting which format we use for a given
    // SkColorType.

    std::fill_n(fColorTypeToFormatTable, kSkColorTypeCnt, wgpu::TextureFormat::Undefined);

    this->setColorType(kAlpha_8_SkColorType,          { wgpu::TextureFormat::R8Unorm });
    this->setColorType(kRGBA_8888_SkColorType,        { wgpu::TextureFormat::RGBA8Unorm });
    this->setColorType(kRGB_888x_SkColorType,
                       {wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureFormat::BGRA8Unorm});
    this->setColorType(kBGRA_8888_SkColorType,        { wgpu::TextureFormat::BGRA8Unorm });
    this->setColorType(kGray_8_SkColorType,           { wgpu::TextureFormat::R8Unorm });
    this->setColorType(kR8_unorm_SkColorType,         { wgpu::TextureFormat::R8Unorm });
    this->setColorType(kRGBA_F16_SkColorType,         { wgpu::TextureFormat::RGBA16Float });
    this->setColorType(kA16_float_SkColorType,        { wgpu::TextureFormat::R16Float });
    this->setColorType(kR8G8_unorm_SkColorType,       { wgpu::TextureFormat::RG8Unorm });
    this->setColorType(kRGBA_1010102_SkColorType,     { wgpu::TextureFormat::RGB10A2Unorm });
    this->setColorType(kR16G16_float_SkColorType,     { wgpu::TextureFormat::RG16Float });

#if !defined(__EMSCRIPTEN__)
    this->setColorType(kA16_unorm_SkColorType,        { wgpu::TextureFormat::R16Unorm });
    this->setColorType(kR16G16_unorm_SkColorType,     { wgpu::TextureFormat::RG16Unorm });
#endif
}

// static
size_t DawnCaps::GetFormatIndex(wgpu::TextureFormat format) {
    for (size_t i = 0; i < std::size(kFormats); ++i) {
        if (format == kFormats[i]) {
            return i;
        }
        if (kFormats[i] == wgpu::TextureFormat::Undefined) {
            SkDEBUGFAILF("Unsupported wgpu::TextureFormat: %d\n", static_cast<int>(format));
            return i;
        }
    }
    SkUNREACHABLE;
    return 0;
}

void DawnCaps::setColorType(SkColorType colorType,
                            std::initializer_list<wgpu::TextureFormat> formats) {
    static_assert(std::size(kFormats) == kFormatCnt,
                  "Size is not same for DawnCaps::fFormatTable and kFormats");
    int idx = static_cast<int>(colorType);
    for (auto it = formats.begin(); it != formats.end(); ++it) {
        const auto& info = this->getFormatInfo(*it);
        for (int i = 0; i < info.fColorTypeInfoCount; ++i) {
            if (info.fColorTypeInfos[i].fColorType == colorType) {
                fColorTypeToFormatTable[idx] = *it;
                return;
            }
        }
    }
}

uint64_t DawnCaps::getRenderPassDescKeyForPipeline(const RenderPassDesc& renderPassDesc) const {
    DawnTextureInfo colorInfo, depthStencilInfo;
    renderPassDesc.fColorAttachment.fTextureInfo.getDawnTextureInfo(&colorInfo);
    renderPassDesc.fDepthStencilAttachment.fTextureInfo.getDawnTextureInfo(&depthStencilInfo);
    SkASSERT(static_cast<uint32_t>(colorInfo.getViewFormat()) <= 0xffff &&
             static_cast<uint32_t>(depthStencilInfo.getViewFormat()) <= 0xffff &&
             colorInfo.fSampleCount < 0x7fff);

    // Note: if Dawn supports ExpandResolveTexture load op and the render pass uses it to load
    // the resolve texture, a render pipeline will need to be created with
    // wgpu::ColorTargetStateExpandResolveTextureDawn chained struct in order to be compatible.
    // Hence a render pipeline created for a render pass using ExpandResolveTexture load op will
    // be different from the one created for a render pass not using that load op.
    // So we need to include a bit flag to differentiate the two kinds of pipelines.
    // Also avoid returning a cached pipeline that is not compatible with the render pass using
    // ExpandResolveTexture load op and vice versa.
    const bool shouldIncludeLoadResolveAttachmentBit = this->resolveTextureLoadOp().has_value();
    uint32_t loadResolveAttachmentKey = 0;
    if (shouldIncludeLoadResolveAttachmentBit &&
        renderPassDesc.fColorResolveAttachment.fTextureInfo.isValid() &&
        renderPassDesc.fColorResolveAttachment.fLoadOp == LoadOp::kLoad) {
        loadResolveAttachmentKey = 1;
    }

    uint32_t colorAttachmentKey = static_cast<uint32_t>(colorInfo.getViewFormat()) << 16 |
                                  colorInfo.fSampleCount << 1 | loadResolveAttachmentKey;

    uint32_t dsAttachmentKey = static_cast<uint32_t>(depthStencilInfo.getViewFormat()) << 16 |
                               depthStencilInfo.fSampleCount;
    return (((uint64_t)colorAttachmentKey) << 32) | dsAttachmentKey;
}

UniqueKey DawnCaps::makeGraphicsPipelineKey(const GraphicsPipelineDesc& pipelineDesc,
                                            const RenderPassDesc& renderPassDesc) const {
    UniqueKey pipelineKey;
    {
        static const skgpu::UniqueKey::Domain kGraphicsPipelineDomain = UniqueKey::GenerateDomain();
        // 5 uint32_t's (render step id, paint id, uint64 RenderPass desc, uint16 write swizzle)
        UniqueKey::Builder builder(&pipelineKey, kGraphicsPipelineDomain, 5, "GraphicsPipeline");
        // add GraphicsPipelineDesc key
        builder[0] = pipelineDesc.renderStepID();
        builder[1] = pipelineDesc.paintParamsID().asUInt();

        // Add RenderPassDesc key.
        uint64_t renderPassKey = this->getRenderPassDescKeyForPipeline(renderPassDesc);
        builder[2] = renderPassKey & 0xFFFFFFFF;
        builder[3] = (renderPassKey >> 32) & 0xFFFFFFFF;
        builder[4] = renderPassDesc.fWriteSwizzle.asKey();
        builder.finish();
    }

    return pipelineKey;
}

UniqueKey DawnCaps::makeComputePipelineKey(const ComputePipelineDesc& pipelineDesc) const {
    UniqueKey pipelineKey;
    {
        static const skgpu::UniqueKey::Domain kComputePipelineDomain = UniqueKey::GenerateDomain();
        // The key is made up of a single uint32_t corresponding to the compute step ID.
        UniqueKey::Builder builder(&pipelineKey, kComputePipelineDomain, 1, "ComputePipeline");
        builder[0] = pipelineDesc.computeStep()->uniqueID();

        // TODO(b/240615224): The local work group size should factor into the key here since it is
        // specified in the shader text on Dawn/SPIR-V. This is not a problem right now since
        // ComputeSteps don't vary their workgroup size dynamically.

        builder.finish();
    }
    return pipelineKey;
}

void DawnCaps::buildKeyForTexture(SkISize dimensions,
                                  const TextureInfo& info,
                                  ResourceType type,
                                  Shareable shareable,
                                  GraphiteResourceKey* key) const {
    const DawnTextureSpec& dawnSpec = info.dawnTextureSpec();

    SkASSERT(!dimensions.isEmpty());

    SkASSERT(dawnSpec.getViewFormat() != wgpu::TextureFormat::Undefined);
    uint32_t formatKey = static_cast<uint32_t>(dawnSpec.getViewFormat());

    uint32_t samplesKey = SamplesToKey(info.numSamples());
    // We don't have to key the number of mip levels because it is inherit in the combination of
    // isMipped and dimensions.
    bool isMipped = info.mipmapped() == Mipmapped::kYes;

    // Confirm all the below parts of the key can fit in a single uint32_t. The sum of the shift
    // amounts in the asserts must be less than or equal to 32.
    SkASSERT(samplesKey                             < (1u << 3));  // sample key is first 3 bits
    SkASSERT(static_cast<uint32_t>(isMipped)        < (1u << 1));  // isMapped is 4th bit
    SkASSERT(static_cast<uint32_t>(dawnSpec.fUsage) < (1u << 28)); // usage is remaining 28 bits

    // We need two uint32_ts for dimensions, 1 for format, and 1 for the rest of the key;
    static int kNum32DataCnt = 2 + 1 + 1;

    GraphiteResourceKey::Builder builder(key, type, kNum32DataCnt, shareable);

    builder[0] = dimensions.width();
    builder[1] = dimensions.height();
    builder[2] = formatKey;
    builder[3] = (samplesKey                                   << 0) |
                 (static_cast<uint32_t>(isMipped)              << 3) |
                 (static_cast<uint32_t>(dawnSpec.fUsage)       << 4);
}

} // namespace skgpu::graphite
