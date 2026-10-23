#include "platform/windows/scaler.hpp"

#include <d3dcompiler.h>

#include <string>

using Microsoft::WRL::ComPtr;

namespace rec::win {
namespace {
// One pass resamples along one axis. Each output pixel's center maps to a source position;
// the Lanczos-3 window, stretched by the scale factor when shrinking, weighs the source pixels
// around it. Taps outside the rectangle repeat its edge pixels.
constexpr char kShaders[] = R"(
Texture2D<float4> source : register(t0);
cbuffer Resample : register(b0) {
    float2 origin;     // Top-left source pixel of the rectangle.
    float2 last;       // Bottom-right source pixel of the rectangle.
    float scale;       // Source pixels per output pixel along the pass.
    float horizontal;  // 1 across, 0 down.
    float2 unused;
};
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
static const float PI = 3.14159265;
float lanczos3(float x) {
    x = abs(x);
    if (x < 1e-4) return 1;
    if (x >= 3) return 0;
    float px = PI * x;
    return 3 * sin(px) * sin(px / 3) / (px * px);
}
float4 ps(float4 position : SV_Position) : SV_Target {
    bool across = horizontal > 0.5;
    float2 pixel = floor(position.xy);
    float center = (across ? origin.x + (pixel.x + 0.5) * scale : origin.y + (pixel.y + 0.5) * scale);
    float stretch = max(scale, 1.0);
    int first = (int)floor(center - 3 * stretch);
    int end = (int)ceil(center + 3 * stretch);
    int lo = (int)(across ? origin.x : origin.y), hi = (int)(across ? last.x : last.y);
    int fixedAt = (int)(across ? origin.y + pixel.y : origin.x + pixel.x);
    float4 sum = 0;
    float total = 0;
    [loop] for (int k = first; k <= end && k - first < 192; ++k) {
        float w = lanczos3((k + 0.5 - center) / stretch);
        int at = clamp(k, lo, hi);
        sum += w * source.Load(int3(across ? int2(at, fixedAt) : int2(fixedAt, at), 0));
        total += w;
    }
    return float4(sum.rgb / total, 1);
}
)";
} // namespace

Result LanczosScaler::start(ID3D11Device* device) {
    device_ = device;
    ComPtr<ID3DBlob> vs, ps, errors;
    HRESULT hr = D3DCompile(kShaders, sizeof kShaders - 1, "lanczos", nullptr, nullptr, "vs", "vs_4_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors);
    if (SUCCEEDED(hr))
        hr = D3DCompile(kShaders, sizeof kShaders - 1, "lanczos", nullptr, nullptr, "ps", "ps_4_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors);
    if (FAILED(hr)) {
        std::string message = "could not compile the Lanczos scaler";
        if (errors) message += ": " + std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        return {message};
    }
    hr = device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_);
    if (SUCCEEDED(hr)) hr = device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_);
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 8 * sizeof(float);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) hr = device_->CreateBuffer(&buffer, nullptr, &constants_);
    if (FAILED(hr)) return {"could not create the Lanczos scaler"};
    return {};
}

bool LanczosScaler::ensure(UINT rows, UINT width, UINT height) {
    const auto make = [&](DXGI_FORMAT format, UINT w, UINT h, ComPtr<ID3D11Texture2D>& texture,
                          ComPtr<ID3D11RenderTargetView>& target, ComPtr<ID3D11ShaderResourceView>* view) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texture.Reset();
        target.Reset();
        if (view) view->Reset();
        return SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &texture)) &&
               SUCCEEDED(device_->CreateRenderTargetView(texture.Get(), nullptr, &target)) &&
               (!view || SUCCEEDED(device_->CreateShaderResourceView(texture.Get(), nullptr, view->ReleaseAndGetAddressOf())));
    };
    if (across_width_ != width || across_height_ != rows) {
        across_width_ = across_height_ = 0;
        // Half floats keep the first pass's overshoot and precision for the second.
        if (!make(DXGI_FORMAT_R16G16B16A16_FLOAT, width, rows, across_, across_target_, &across_view_)) return false;
        across_width_ = width;
        across_height_ = rows;
    }
    if (result_width_ != width || result_height_ != height) {
        result_width_ = result_height_ = 0;
        if (!make(DXGI_FORMAT_B8G8R8A8_UNORM, width, height, result_, result_target_, nullptr)) return false;
        result_width_ = width;
        result_height_ = height;
    }
    return true;
}

void LanczosScaler::pass(ID3D11DeviceContext* context, ID3D11ShaderResourceView* input, ID3D11RenderTargetView* output,
                         UINT width, UINT height, const float (&constants)[8]) {
    context->UpdateSubresource(constants_.Get(), 0, nullptr, constants, 0, 0);
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    context->OMSetRenderTargets(1, &output, nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);
    context->RSSetState(nullptr);
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_.Get(), nullptr, 0);
    context->PSSetShader(pixel_.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context->PSSetShaderResources(0, 1, &input);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* no_input = nullptr;
    context->PSSetShaderResources(0, 1, &no_input);
    context->OMSetRenderTargets(0, nullptr, nullptr);
}

ID3D11Texture2D* LanczosScaler::scale(ID3D11DeviceContext* context, ID3D11Texture2D* source, const RECT& from, UINT width,
                                      UINT height) {
    if (from.right - from.left < 1 || from.bottom - from.top < 1 || !width || !height) return nullptr;
    const auto columns = static_cast<UINT>(from.right - from.left), rows = static_cast<UINT>(from.bottom - from.top);
    if (!ensure(rows, width, height)) return nullptr;
    if (source_.Get() != source) {
        source_view_.Reset();
        source_ = source;
        if (FAILED(device_->CreateShaderResourceView(source, nullptr, &source_view_))) {
            source_.Reset();
            return nullptr;
        }
    }
    const float across[8] = {static_cast<float>(from.left), static_cast<float>(from.top), static_cast<float>(from.right - 1),
                             static_cast<float>(from.bottom - 1), static_cast<float>(columns) / width, 1, 0, 0};
    const float down[8] = {0, 0, static_cast<float>(width - 1), static_cast<float>(rows - 1),
                           static_cast<float>(rows) / height, 0, 0, 0};
    pass(context, source_view_.Get(), across_target_.Get(), width, rows, across);
    pass(context, across_view_.Get(), result_target_.Get(), width, height, down);
    return result_.Get();
}

} // namespace rec::win
