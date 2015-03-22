/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CanvasLayerD3D10.h"

#include "../d3d9/Nv3DVUtils.h"
#include "gfxWindowsSurface.h"
#include "gfxWindowsPlatform.h"
#include "SharedSurfaceANGLE.h"
#include "SharedSurfaceGL.h"
#include "gfxContext.h"
#include "GLContext.h"
#include "gfxPrefs.h"

namespace mozilla {
namespace layers {

using namespace mozilla::gl;
using namespace mozilla::gfx;

CanvasLayerD3D10::CanvasLayerD3D10(LayerManagerD3D10 *aManager)
  : CanvasLayer(aManager, nullptr)
  , LayerD3D10(aManager)
  , mDataIsPremultiplied(true)
  , mOriginPos(gl::OriginPos::TopLeft)
  , mHasAlpha(true)
{
    mImplData = static_cast<LayerD3D10*>(this);
}

CanvasLayerD3D10::~CanvasLayerD3D10()
{
}

void
CanvasLayerD3D10::Initialize(const Data& aData)
{
  NS_ASSERTION(mSurface == nullptr, "BasicCanvasLayer::Initialize called twice!");

  if (aData.mGLContext) {
    mGLContext = aData.mGLContext;
    NS_ASSERTION(mGLContext->IsOffscreen(), "Canvas GLContext must be offscreen.");
    mDataIsPremultiplied = aData.mIsGLAlphaPremult;
    mOriginPos = gl::OriginPos::TopLeft;

    GLScreenBuffer* screen = mGLContext->Screen();

    UniquePtr<SurfaceFactory> factory = nullptr;
    if (!gfxPrefs::WebGLForceLayersReadback()) {
      if (mGLContext->IsANGLE()) {
        factory = SurfaceFactory_ANGLEShareHandle::Create(mGLContext,
                                                          screen->mCaps);
      }
    }

    if (factory) {
      screen->Morph(Move(factory));
    }
  } else if (aData.mDrawTarget) {
    mDrawTarget = aData.mDrawTarget;
    void *texture = mDrawTarget->GetNativeSurface(NativeSurfaceType::D3D10_TEXTURE);

    if (texture) {
      mTexture = static_cast<ID3D10Texture2D*>(texture);

      NS_ASSERTION(!aData.mGLContext,
                   "CanvasLayer can't have both DrawTarget and WebGLContext/Surface");

      mBounds.SetRect(0, 0, aData.mSize.width, aData.mSize.height);
      device()->CreateShaderResourceView(mTexture, nullptr, getter_AddRefs(mSRView));
      return;
    }

    // XXX we should store mDrawTarget and use it directly in UpdateSurface,
    // bypassing Thebes
    mSurface = mDrawTarget->Snapshot();
  } else {
    MOZ_CRASH("CanvasLayer created without mSurface, mDrawTarget or mGLContext?");
  }

  mBounds.SetRect(0, 0, aData.mSize.width, aData.mSize.height);
  mIsD2DTexture = false;

  // Create a texture in case we need to readback.
  CD3D10_TEXTURE2D_DESC desc(DXGI_FORMAT_B8G8R8A8_UNORM, mBounds.width, mBounds.height, 1, 1);
  desc.Usage = D3D10_USAGE_DYNAMIC;
  desc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;

  HRESULT hr = device()->CreateTexture2D(&desc, nullptr, getter_AddRefs(mTexture));
  if (FAILED(hr)) {
    NS_WARNING("Failed to create texture for CanvasLayer!");
    return;
  }

  device()->CreateShaderResourceView(mTexture, nullptr, getter_AddRefs(mUploadSRView));
}

void
CanvasLayerD3D10::UpdateSurface()
{
  if (!IsDirty())
    return;
  Painted();

  if (mDrawTarget) {
    mDrawTarget->Flush();
  } else if (mIsD2DTexture) {
    return;
  }

  if (!mTexture) {
    return;
  }

  SharedSurface* surf = nullptr;
  if (mGLContext) {
    auto screen = mGLContext->Screen();
    MOZ_ASSERT(screen);

    surf = screen->Front()->Surf();
    if (!surf)
      return;
    surf->WaitSync();

    if (surf->mType == SharedSurfaceType::EGLSurfaceANGLE) {
      SharedSurface_ANGLEShareHandle* shareSurf = SharedSurface_ANGLEShareHandle::Cast(surf);
      HANDLE shareHandle = shareSurf->GetShareHandle();

      HRESULT hr = device()->OpenSharedResource(shareHandle,
                                                __uuidof(ID3D10Texture2D),
                                                getter_AddRefs(mTexture));
      if (FAILED(hr))
        return;

      hr = device()->CreateShaderResourceView(mTexture,
                                              nullptr,
                                              getter_AddRefs(mSRView));
      if (FAILED(hr))
        return;

      return;
    }
  }

  D3D10_MAPPED_TEXTURE2D map;
  HRESULT hr = mTexture->Map(0, D3D10_MAP_WRITE_DISCARD, 0, &map);

  if (FAILED(hr)) {
    gfxWarning() << "Failed to lock CanvasLayer texture.";
    return;
  }

  RefPtr<DrawTarget> destTarget =
    Factory::CreateDrawTargetForD3D10Texture(mTexture,
                                             SurfaceFormat::R8G8B8A8);

  if (!destTarget) {
    gfxWarning() << "Invalid D3D10 texture target R8G8B8A8";
    return;
  }

  if (surf) {
    if (!ReadbackSharedSurface(surf, destTarget)) {
      gfxWarning() << "Failed to readback into texture.";
    }
  } else if (mSurface) {
    Rect r(Point(0, 0), ToRect(mBounds).Size());
    destTarget->DrawSurface(mSurface, r, r, DrawSurfaceOptions(),
                            DrawOptions(1.0F, CompositionOp::OP_SOURCE));
  }

  mTexture->Unmap(0);
  mSRView = mUploadSRView;
}

Layer*
CanvasLayerD3D10::GetLayer()
{
  return this;
}

void
CanvasLayerD3D10::RenderLayer()
{
  FirePreTransactionCallback();
  UpdateSurface();
  FireDidTransactionCallback();

  if (!mTexture)
    return;

  nsIntRect visibleRect = mVisibleRegion.GetBounds();

  SetEffectTransformAndOpacity();

  uint8_t shaderFlags = 0;
  shaderFlags |= LoadMaskTexture();
  shaderFlags |= mDataIsPremultiplied
                ? SHADER_PREMUL : SHADER_NON_PREMUL | SHADER_RGBA;
  shaderFlags |= mHasAlpha ? SHADER_RGBA : SHADER_RGB;
  shaderFlags |= mFilter == GraphicsFilter::FILTER_NEAREST
                ? SHADER_POINT : SHADER_LINEAR;
  ID3D10EffectTechnique* technique = SelectShader(shaderFlags);

  if (mSRView) {
    effect()->GetVariableByName("tRGB")->AsShaderResource()->SetResource(mSRView);
  }

  effect()->GetVariableByName("vLayerQuad")->AsVector()->SetFloatVector(
    ShaderConstantRectD3D10(
      (float)mBounds.x,
      (float)mBounds.y,
      (float)mBounds.width,
      (float)mBounds.height)
    );

  const bool needsYFlip = (mOriginPos == gl::OriginPos::BottomLeft);

  if (needsYFlip) {
    effect()->GetVariableByName("vTextureCoords")->AsVector()->SetFloatVector(
      ShaderConstantRectD3D10(
        0,
        1.0f,
        1.0f,
        -1.0f)
      );
  }

  technique->GetPassByIndex(0)->Apply(0);
  device()->Draw(4, 0);

  if (needsYFlip) {
    effect()->GetVariableByName("vTextureCoords")->AsVector()->
      SetFloatVector(ShaderConstantRectD3D10(0, 0, 1.0f, 1.0f));
  }
}

} /* namespace layers */
} /* namespace mozilla */