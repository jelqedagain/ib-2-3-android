#include "win/image.h"
#include <wincodec.h>

namespace win {

namespace {

template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() { return p; }
};

bool decode(const std::wstring& path, const GUID& format, std::vector<u8>& out, int& w, int& h) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC works in either apartment
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.p, format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT uw = 0, uh = 0;
    if (FAILED(converter->GetSize(&uw, &uh)) || !uw || !uh) return false;
    out.resize((size_t)uw * uh * 4);
    if (FAILED(converter->CopyPixels(nullptr, uw * 4, (UINT)out.size(), out.data()))) return false;
    w = (int)uw;
    h = (int)uh;
    return true;
}

}  // namespace

bool load_image_rgba(const std::wstring& path, std::vector<u8>& rgba, int& w, int& h) {
    return decode(path, GUID_WICPixelFormat32bppRGBA, rgba, w, h);
}

HICON load_icon(const std::wstring& path) {
    std::vector<u8> bgra;
    int w = 0, h = 0;
    if (!decode(path, GUID_WICPixelFormat32bppPBGRA, bgra, w, h)) return nullptr;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w;
    bi.bV5Height = -h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00ff0000;
    bi.bV5GreenMask = 0x0000ff00;
    bi.bV5BlueMask = 0x000000ff;
    bi.bV5AlphaMask = 0xff000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    memcpy(bits, bgra.data(), bgra.size());
    HBITMAP mask = CreateBitmap(w, h, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

}  // namespace win
