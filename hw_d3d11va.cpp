#include <windows.h>
#include <windowsx.h>

#include <dxgi.h>
#include <d3d11.h>
#include <dxgi1_3.h>

#include <thread>
#include <string>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_d3d11va.h>
}

ID3D11Device*                      g_pD3D11Device;
ID3D11DeviceContext*               g_pD3D11DeviceContext;
ID3D11VideoDevice*                 g_pD3D11VideoDevice;
ID3D11VideoContext*                g_pD3D11VideoContext;
IDXGISwapChain2*                   g_pSwapChain2;
ID3D11VideoProcessorEnumerator*    g_pD3D11VideoProcessorEnumerator;
ID3D11VideoProcessor*              g_pD3D11VideoProcessor;

#define SAFE_RELEASE(X)                                   \
    if ((X)) {                                            \
        (X)->Release();                                   \
        X = NULL;                                         \
    }

std::thread           g_thDecodeThread;
bool                  g_bDecodeThreadCanRun = false;
AVBufferRef*          g_pBufferRef;
enum AVPixelFormat    g_pixelFormat;

LRESULT CALLBACK      WindowProc(HWND, UINT, WPARAM, LPARAM);
void                  OpenStream(HWND, const std::string);
int                   InitHWDecoder(HWND, AVCodecContext*, const enum AVHWDeviceType);
HRESULT               InitD3D11(HWND);
void                  CleanD3D11();
int                   DecodeFrame(HWND, AVCodecContext*, AVPacket*);
void                  RenderFrame(HWND, AVCodecContext*, AVFrame*);
enum AVPixelFormat    GetHWFormat(AVCodecContext*, const enum AVPixelFormat*);

int WINAPI WinMain(HINSTANCE hInstance,
                   HINSTANCE hPrevInstance,
                   LPSTR     lpCmdLine,
                   int       nCmdShow)
{
    HWND hWnd;
    WNDCLASSEX wc;

    ZeroMemory(&wc, sizeof(WNDCLASSEX));

    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)COLOR_WINDOW;
    wc.lpszClassName = L"WindowClass";

    RegisterClassEx(&wc);

    RECT wr = {0, 0, 800, 600};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    hWnd = CreateWindowEx(NULL,
                          L"WindowClass",
                          L"FFmpeg Direct3D Video API",
                          WS_OVERLAPPEDWINDOW,
                          300,
                          300,
                          wr.right - wr.left,
                          wr.bottom - wr.top,
                          NULL,
                          NULL,
                          hInstance,
                          NULL);

    ShowWindow(hWnd, nCmdShow);

    MSG msg;
    while (true)
    {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);

            if(msg.message == WM_QUIT)
                break;
        }
    }
    return msg.wParam;
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            g_bDecodeThreadCanRun = TRUE;
            // "D:/resources/Forrest_Gump_IMAX.mp4"
            // "https://gstreamer.freedesktop.org/data/media/sintel_trailer-480p.webm"
            // "D:/resources/peru_7680x4320.mp4"
            g_thDecodeThread = std::thread(OpenStream, hWnd, "D:/resources/peru_7680x4320.mp4");
        } break;
        case WM_DESTROY:
        {
            g_bDecodeThreadCanRun = FALSE;
            if (g_thDecodeThread.joinable())
                g_thDecodeThread.join();
            PostQuitMessage(0);
            return 0;
        } break;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void OpenStream(HWND hWnd, const std::string strUrl)
{
    int ret = 0;

    AVFormatContext* pFormatContext = nullptr;
    ret = avformat_open_input(&pFormatContext, strUrl.c_str(), nullptr, nullptr);
    if (ret < 0)
    {
        MessageBox(NULL, L"avformat_open_input", L"Error", MB_ICONERROR | MB_OK);
        return;
    }

    ret = avformat_find_stream_info(pFormatContext, nullptr);
    if (ret < 0)
    {
        MessageBox(NULL, L"avformat_find_stream_info", L"Error", MB_ICONERROR | MB_OK);
        return;
    }

    av_dump_format(pFormatContext, 0, strUrl.c_str(), 0);

    int iVideo = -1;
    for (unsigned int i = 0; i < pFormatContext->nb_streams; i++)
    {
        if (pFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            iVideo = static_cast<int>(i);
        }
    }
    if (iVideo == -1)
    {
        MessageBox(NULL, L"Can't find video stream", L"Error", MB_ICONERROR | MB_OK);
        return;
    }

    const AVCodec* pCodec = avcodec_find_decoder(pFormatContext->streams[iVideo]->codecpar->codec_id);
    if (!pCodec)
    {
        MessageBox(NULL, L"avcodec_find_decoder", L"Error", MB_ICONERROR | MB_OK);
        return;
    }

    AVCodecContext* pCodecCtx = avcodec_alloc_context3(pCodec);
    if (!pCodecCtx)
    {
        MessageBox(NULL, L"avcodec_alloc_context3", L"Error", MB_ICONERROR | MB_OK);
        return;
    }

    if (avcodec_parameters_to_context(pCodecCtx, pFormatContext->streams[iVideo]->codecpar) < 0)
    {
        MessageBox(NULL, L"avcodec_parameters_to_context", L"Error", MB_ICONERROR | MB_OK);
        return;
    }

    enum AVHWDeviceType type = av_hwdevice_find_type_by_name("d3d11va");

    if (type == AV_HWDEVICE_TYPE_NONE)
    {
        MessageBox(NULL, L"av_hwdevice_find_type_by_name", L"Error", MB_ICONERROR | MB_OK);
        return;
    }

    for (int i = 0;; i++)
    {
        const AVCodecHWConfig* pConfig = avcodec_get_hw_config(pCodec, i);
        if (!pConfig)
        {
            MessageBox(NULL, L"avcodec_get_hw_config", L"Error", MB_ICONERROR | MB_OK);
            return;
        }
        if (pConfig->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            pConfig->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX &&
            pConfig->device_type == type)
        {
            g_pixelFormat = pConfig->pix_fmt;
            break;
        }
    }

    pCodecCtx->get_format = GetHWFormat;

    if (InitHWDecoder(hWnd, pCodecCtx, type) < 0)
    {
        MessageBox(NULL, L"InitHWDecoder", L"Error", MB_ICONERROR | MB_OK);
        return;
    }

    pCodecCtx->pix_fmt = g_pixelFormat;

    ret = avcodec_open2(pCodecCtx, pCodec, NULL);
    if (ret < 0)
    {
        MessageBox(NULL, L"avcodec_open2", L"Error", MB_ICONERROR | MB_OK);
        return;
    }

    AVPacket* pPacket = pPacket = av_packet_alloc();
    if (!pPacket)
    {
        MessageBox(NULL, L"av_packet_alloc", L"Error", MB_ICONERROR | MB_OK);
        return;
    }

    while (g_bDecodeThreadCanRun && ret >= 0)
    {
        if ((ret = av_read_frame(pFormatContext, pPacket)) < 0)
            break;

        if (pPacket->stream_index == iVideo)
            ret = DecodeFrame(hWnd, pCodecCtx, pPacket);

        av_packet_unref(pPacket);
    }

    ret = DecodeFrame(hWnd, pCodecCtx, NULL);

    av_packet_free(&pPacket);
    avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatContext);

    CleanD3D11();
}

HRESULT InitD3D11(HWND hWnd)
{
    HRESULT hr = S_OK;

    IDXGISwapChain1* pSwapChain1 = nullptr;
    ID3D11RenderTargetView* pRenderTargetView = nullptr;
    ID3D11Texture2D* pBackBuffer = nullptr;

    IDXGIDevice1* pDXGIdevice = nullptr;
    hr = g_pD3D11Device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&pDXGIdevice);
    if (FAILED(hr))
        goto done;

    IDXGIAdapter* pDXGIAdapter = nullptr;
    hr = pDXGIdevice->GetParent(__uuidof(IDXGIAdapter), (void**)&pDXGIAdapter);
    if (FAILED(hr))
        goto done;

    IDXGIFactory2* pIDXGIFactory3 = nullptr;
    hr = pDXGIAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&pIDXGIFactory3);
    if (FAILED(hr))
        goto done;

    IDXGIOutput* pDXGIOutput = nullptr;
    hr = pDXGIAdapter->EnumOutputs(0, &pDXGIOutput);
    if (FAILED(hr))
        goto done;

    RECT rect;
    GetClientRect(hWnd, &rect);

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
    ZeroMemory(&swapChainDesc, sizeof(swapChainDesc));
    swapChainDesc.Width               = rect.right - rect.left;
    swapChainDesc.Height              = rect.bottom - rect.top;
    swapChainDesc.Format              = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.Stereo              = FALSE;
    swapChainDesc.SampleDesc.Count    = 1;
    swapChainDesc.SampleDesc.Quality  = 0;
    swapChainDesc.BufferUsage         = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount         = 2;
    // swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SwapEffect          = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapChainDesc.Scaling             = DXGI_SCALING_STRETCH;
    // swapChainDesc.AlphaMode = DXGI_ALPHA_MODE::DXGI_ALPHA_MODE_IGNORE;
    // swapChainDesc.Flags = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    swapChainDesc.Flags               = 0;

    hr = pIDXGIFactory3->CreateSwapChainForHwnd(g_pD3D11Device, hWnd, &swapChainDesc, NULL, NULL, &pSwapChain1);
    if (FAILED(hr)) goto done;

    g_pSwapChain2 = (IDXGISwapChain2*)pSwapChain1;

    // DXGI_SWAP_CHAIN_DESC1 swapChainDesc2;
    // pSwapChain1->GetDesc1(&swapChainDesc2);

    g_pSwapChain2->SetMaximumFrameLatency(1);

    // IF_FAILED_THROW(m_swapchain2->SetFullscreenState(TRUE, NULL)); //full screen
    // ResizeSwapChain();

    hr = g_pSwapChain2->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pBackBuffer));
    if (FAILED(hr))
        goto done;

    // Create a render target view
    hr = g_pD3D11Device->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView);
    if (FAILED(hr))
        goto done;

    // Set new render target
    g_pD3D11DeviceContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);

    D3D11_VIEWPORT VP;
    VP.Width        = swapChainDesc.Width;
    VP.Height       = swapChainDesc.Height;
    VP.MinDepth     = 0.0f;
    VP.MaxDepth     = 1.0f;
    VP.TopLeftX     = 0;
    VP.TopLeftY     = 0;
    g_pD3D11DeviceContext->RSSetViewports(1, &VP);

done:
    if (FAILED(hr))
        SAFE_RELEASE(pSwapChain1);

    SAFE_RELEASE(pBackBuffer);
    SAFE_RELEASE(pRenderTargetView);
    SAFE_RELEASE(pDXGIOutput);
    SAFE_RELEASE(pIDXGIFactory3);
    SAFE_RELEASE(pDXGIAdapter);
    SAFE_RELEASE(pDXGIdevice);

    return hr;
}

void CleanD3D11()
{
    SAFE_RELEASE(g_pD3D11VideoProcessorEnumerator);
    SAFE_RELEASE(g_pD3D11VideoProcessor);
    SAFE_RELEASE(g_pSwapChain2);
}

enum AVPixelFormat GetHWFormat(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts)
{
    const enum AVPixelFormat* p;

    for (p = pix_fmts; *p != -1; p++)
    {
        if (*p == g_pixelFormat)
            return *p;
    }

    return AV_PIX_FMT_NONE;
}

int InitHWDecoder(HWND hWnd, AVCodecContext* pCodecContext, const enum AVHWDeviceType type)
{
    AVDictionary* pOpts = nullptr;
    char* device = nullptr;
    int ret = 0;

    if (type == AV_HWDEVICE_TYPE_D3D11VA)
    {
        device = "0";
    }
    else
    {
        return -1;
    }

    if ((ret = av_hwdevice_ctx_create(&g_pBufferRef, type, device, pOpts, 0)) < 0)
    {
        MessageBox(NULL, L"av_hwdevice_ctx_create", L"Error", MB_ICONERROR | MB_OK);
        av_dict_free(&pOpts);
        return ret;
    }

    pCodecContext->hw_device_ctx = av_buffer_ref(g_pBufferRef);

    AVHWDeviceContext* hw_device_ctx = (AVHWDeviceContext*)pCodecContext->hw_device_ctx->data;
    AVD3D11VADeviceContext* d3d11_device_ctx = (AVD3D11VADeviceContext*)hw_device_ctx->hwctx;

    g_pD3D11Device = d3d11_device_ctx->device;
    g_pD3D11DeviceContext = d3d11_device_ctx->device_context;
    g_pD3D11VideoDevice = d3d11_device_ctx->video_device;
    g_pD3D11VideoContext = d3d11_device_ctx->video_context;

    if (FAILED(InitD3D11(hWnd)))
    {
        MessageBox(NULL, L"av_hwdevice_ctx_create", L"Error", MB_ICONERROR | MB_OK);
        av_dict_free(&pOpts);
        return ret;
    }

    av_dict_free(&pOpts);

    return ret;
}

void RenderFrame(HWND hWnd, AVCodecContext* avctx, AVFrame* frame)
{
    HRESULT hr;
    D3D11_TEXTURE2D_DESC texture_desc;
    D3D11_TEXTURE2D_DESC bktexture_desc;
    ID3D11RenderTargetView* pRenderTargetView = nullptr;
    ID3D11VideoProcessorInputView* pD3D11VideoProcessorInputViewIn = nullptr;
    ID3D11VideoProcessorOutputView* pD3D11VideoProcessorOutputView = nullptr;
    ID3D11Texture2D* pDXGIBackBuffer = nullptr;

    if (frame == nullptr || frame->format != AV_PIX_FMT_D3D11)
        return;

    int index = (intptr_t)frame->data[1];
    ID3D11Texture2D* pD3D11Texture2D = (ID3D11Texture2D*)frame->data[0];

    pD3D11Texture2D->GetDesc(&texture_desc);

    hr = g_pSwapChain2->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pDXGIBackBuffer);
    if (FAILED(hr))
        return;

    pDXGIBackBuffer->GetDesc(&bktexture_desc);

#if 1
    RECT rect;
    GetClientRect(hWnd, &rect);

    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;

    if (w != bktexture_desc.Width || h != bktexture_desc.Height)
    {

        // ResizeSwapChain();
        hr = g_pSwapChain2->ResizeBuffers(
            2,
            w,
            h,
            bktexture_desc.Format,
            0
        );
        if (SUCCEEDED(hr))
        {
#if 1
            pDXGIBackBuffer->GetDesc(&bktexture_desc);
#else
            bktexture_desc.Width = w;
            bktexture_desc.Height = h;
#endif
            // Create a render target view
            hr = g_pD3D11Device->CreateRenderTargetView(pDXGIBackBuffer, nullptr, &pRenderTargetView);
            if (FAILED(hr))
                goto done;

            // Set new render target
            g_pD3D11DeviceContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);

            D3D11_VIEWPORT VP;
            ZeroMemory(&VP, sizeof(VP));
            VP.Width = bktexture_desc.Width;
            VP.Height = bktexture_desc.Height;
            VP.MinDepth = 0.0f;
            VP.MaxDepth = 1.0f;
            VP.TopLeftX = 0;
            VP.TopLeftY = 0;
            g_pD3D11DeviceContext->RSSetViewports(1, &VP);

            SAFE_RELEASE(pRenderTargetView);
            SAFE_RELEASE(g_pD3D11VideoProcessorEnumerator);
            SAFE_RELEASE(g_pD3D11VideoProcessor);
        }
    }
#endif

    if (!g_pD3D11VideoProcessorEnumerator || !g_pD3D11VideoProcessor)
    {
        SAFE_RELEASE(g_pD3D11VideoProcessorEnumerator);
        SAFE_RELEASE(g_pD3D11VideoProcessor);

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC ContentDesc;
        ZeroMemory(&ContentDesc, sizeof(ContentDesc));
        ContentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        ContentDesc.InputWidth = texture_desc.Width;
        ContentDesc.InputHeight = texture_desc.Height;
        ContentDesc.OutputWidth = bktexture_desc.Width;
        ContentDesc.OutputHeight = bktexture_desc.Height;
        ContentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        hr = g_pD3D11VideoDevice->CreateVideoProcessorEnumerator(&ContentDesc, &g_pD3D11VideoProcessorEnumerator);
        if (FAILED(hr))
            return;

        UINT uiFlags;
        DXGI_FORMAT VP_Output_Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        hr = g_pD3D11VideoProcessorEnumerator->CheckVideoProcessorFormat(VP_Output_Format, &uiFlags);
        if (FAILED(hr))
            return;

        DXGI_FORMAT VP_input_Format = texture_desc.Format;
        hr = g_pD3D11VideoProcessorEnumerator->CheckVideoProcessorFormat(VP_input_Format, &uiFlags);
        if (FAILED(hr))
            return;

        //  NV12 surface to RGB backbuffer
        RECT srcrc = { 0, 0, (LONG)texture_desc.Width, (LONG)texture_desc.Height };
        RECT destcrc = { 0, 0, (LONG)bktexture_desc.Width, (LONG)bktexture_desc.Height };

        hr = g_pD3D11VideoDevice->CreateVideoProcessor(g_pD3D11VideoProcessorEnumerator, 0, &g_pD3D11VideoProcessor);
        if (FAILED(hr))
            return;

        g_pD3D11VideoContext->VideoProcessorSetStreamFrameFormat(g_pD3D11VideoProcessor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        g_pD3D11VideoContext->VideoProcessorSetStreamOutputRate(g_pD3D11VideoProcessor, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, TRUE, NULL);

        g_pD3D11VideoContext->VideoProcessorSetStreamSourceRect(g_pD3D11VideoProcessor, 0, TRUE, &srcrc);
        g_pD3D11VideoContext->VideoProcessorSetStreamDestRect(g_pD3D11VideoProcessor, 0, TRUE, &destcrc);
        g_pD3D11VideoContext->VideoProcessorSetOutputTargetRect(g_pD3D11VideoProcessor, TRUE, &destcrc);


        D3D11_VIDEO_COLOR color;
        color.YCbCr = { 0.0625f, 0.5f, 0.5f, 0.5f }; // black color
        g_pD3D11VideoContext->VideoProcessorSetOutputBackgroundColor(g_pD3D11VideoProcessor, TRUE, &color);

    }

    //        IF_FAILED_THROW(m_pD3D11Device->CreateRenderTargetView(m_pDXGIBackBuffer, nullptr, &_rtv));
    //        D3D11_VIEWPORT VP;
    //        ZeroMemory(&VP, sizeof(VP));
    //        VP.Width = bktexture_desc.Width;
    //        VP.Height = bktexture_desc.Height;
    //        VP.MinDepth = 0.0f;
    //        VP.MaxDepth = 1.0f;
    //        VP.TopLeftX = 0;
    //        VP.TopLeftY = 0;
    //        m_pD3D11DeviceContext->RSSetViewports(1, &VP);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC pInDesc;
    ZeroMemory(&pInDesc, sizeof(pInDesc));
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC InputViewDesc;

    pInDesc.FourCC = 0;
    pInDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    pInDesc.Texture2D.MipSlice = 0;
    pInDesc.Texture2D.ArraySlice = index;

    hr = g_pD3D11VideoDevice->CreateVideoProcessorInputView(pD3D11Texture2D, g_pD3D11VideoProcessorEnumerator, &pInDesc, &pD3D11VideoProcessorInputViewIn);
    if (FAILED(hr))
        goto done;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC pOutDesc;
    ZeroMemory(&pOutDesc, sizeof(pOutDesc));

    pOutDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    pOutDesc.Texture2D.MipSlice = 0;

    hr = g_pD3D11VideoDevice->CreateVideoProcessorOutputView(pDXGIBackBuffer, g_pD3D11VideoProcessorEnumerator, &pOutDesc, &pD3D11VideoProcessorOutputView);
    if (FAILED(hr))
        goto done;

    D3D11_VIDEO_PROCESSOR_STREAM StreamData;
    ZeroMemory(&StreamData, sizeof(StreamData));
    StreamData.Enable = TRUE;
    StreamData.OutputIndex = 0;
    StreamData.InputFrameOrField = 0;
    StreamData.PastFrames = 0;
    StreamData.FutureFrames = 0;
    StreamData.ppPastSurfaces = nullptr;
    StreamData.ppFutureSurfaces = nullptr;
    StreamData.pInputSurface = pD3D11VideoProcessorInputViewIn;
    StreamData.ppPastSurfacesRight = nullptr;
    StreamData.ppFutureSurfacesRight = nullptr;

    hr = g_pD3D11VideoContext->VideoProcessorBlt(g_pD3D11VideoProcessor, pD3D11VideoProcessorOutputView, 0, 1, &StreamData);
    if (FAILED(hr))
        goto done;

    DXGI_PRESENT_PARAMETERS parameters;
    ZeroMemory(&parameters, sizeof(parameters));

    if (g_pSwapChain2 != NULL)
    {
        // hr = m_swapchain2->Present1(0, DXGI_PRESENT_ALLOW_TEARING, &parameters);
        g_pSwapChain2->Present1(0, DXGI_PRESENT_DO_NOT_WAIT, &parameters);
    }

done:
    // SAFE_RELEASE(hwTexture);
    SAFE_RELEASE(pD3D11VideoProcessorOutputView);
    SAFE_RELEASE(pD3D11VideoProcessorInputViewIn);
    SAFE_RELEASE(pDXGIBackBuffer);
}

int DecodeFrame(HWND hWnd, AVCodecContext* avctx, AVPacket* packet)
{
    AVFrame* frame = NULL;
    int ret;

    ret = avcodec_send_packet(avctx, packet);
    if (ret < 0)
    {
        fprintf(stderr, "Error during decoding\n");
        return ret;
    }

    while (true)
    {
        if (!(frame = av_frame_alloc())) {
            fprintf(stderr, "Can not alloc frame\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = avcodec_receive_frame(avctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            av_frame_free(&frame);
            return 0;
        }
        else if (ret < 0)
        {
            fprintf(stderr, "Error while decoding\n");
            goto fail;
        }

        if (frame->format == AV_PIX_FMT_D3D11)
            RenderFrame(hWnd, avctx, frame);

    fail:
        av_frame_free(&frame);
        if (ret < 0)
            return ret;
    }
}
