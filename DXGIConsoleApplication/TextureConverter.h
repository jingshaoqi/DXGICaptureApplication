/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#pragma once

#include "Com.h"
#include "common\KVMFR.h"

#include <DirectXMath.h>
#include <vector>

typedef std::vector<ID3D11Texture2DPtr> TextureList;

class TextureConverter
{
public:
  TextureConverter();
  ~TextureConverter();

  bool Initialize(
    ID3D11DeviceContextPtr deviceContext,
    ID3D11DevicePtr        device,
    const unsigned int     width,
    const unsigned int     height,
    FrameType              format
  );

  void DeInitialize();

  bool Convert(ID3D11Texture2DPtr texture, TextureList & output);

private:
  struct VS_INPUT
  {
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT2 tex;
  };

  bool IntializeBuffers();

  ID3D11DeviceContextPtr m_deviceContext = NULL;
  ID3D11DevicePtr        m_device = NULL;
  unsigned int           m_width = 0, m_height = 0;
  FrameType              m_format =  FRAME_TYPE_INVALID;

  DXGI_FORMAT                 m_texFormats   [3];
  unsigned int                m_scaleFormats [3];

  ID3D11Texture2DPtr          m_targetTexture[3];
  ID3D11RenderTargetViewPtr   m_renderView   [3];
  ID3D11ShaderResourceViewPtr m_shaderView   [3];

  ID3D11InputLayoutPtr        m_layout =  NULL;
  ID3D11VertexShaderPtr       m_vertexShader = NULL;
  ID3D11PixelShaderPtr        m_psCopy = NULL;
  ID3D11PixelShaderPtr        m_psConversion = NULL;
  ID3D11SamplerStatePtr       m_samplerState = NULL;

  ID3D11BufferPtr             m_vertexBuffer =NULL;
  unsigned int                m_vertexCount =0;
  ID3D11BufferPtr             m_indexBuffer = NULL;
  unsigned int                m_indexCount = 0;
};

