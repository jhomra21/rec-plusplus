#pragma once

#include "rec/rec.hpp"

#include <d3d11.h>
#include <wrl/client.h>

namespace rec::win {

// Lanczos-3 downscaling of a BGRA texture on the GPU, in two separable passes (across, then
// down). The kernel widens with the scale factor, so every source pixel contributes and fine
// detail averages out instead of aliasing. Values are filtered as encoded, like the source.
class LanczosScaler {
public:
    Result start(ID3D11Device* device);
    // Scales the from rectangle of source (which needs a shader resource binding) to width x
    // height and returns the scaler's BGRA texture, valid until the next call, or null. The
    // caller holds the device's multithread lock: the passes set the context's pipeline state.
    ID3D11Texture2D* scale(ID3D11DeviceContext* context, ID3D11Texture2D* source, const RECT& from, UINT width, UINT height);

private:
    void pass(ID3D11DeviceContext* context, ID3D11ShaderResourceView* input, ID3D11RenderTargetView* output, UINT width,
              UINT height, const float (&constants)[8]);
    bool ensure(UINT rows, UINT width, UINT height);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_view_;
    // After the first pass (output width, source rows) and after both.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> across_, result_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> across_view_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> across_target_, result_target_;
    UINT across_width_ = 0, across_height_ = 0, result_width_ = 0, result_height_ = 0;
};

} // namespace rec::win
