// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.


#include "Common.h"

#if defined(HAVE_WX) && HAVE_WX
#include "VideoConfigDialog.h"
#endif // HAVE_WX

#include "../OGL/GLExtensions/GLExtensions.h"
#include "Atomic.h"
#include "SWCommandProcessor.h"
#include "OpcodeDecoder.h"
#include "SWVideoConfig.h"
#include "SWPixelEngine.h"
#include "BPMemLoader.h"
#include "XFMemLoader.h"
#include "Clipper.h"
#include "Rasterizer.h"
#include "SWRenderer.h"
#include "HwRasterizer.h"
#include "LogManager.h"
#include "EfbInterface.h"
#include "DebugUtil.h"
#include "FileUtil.h"
#include "VideoBackend.h"
#include "Core.h"
#include "OpcodeDecoder.h"
#include "SWVertexLoader.h"
#include "SWStatistics.h"
#include "HW/VideoInterface.h"
#include "HW/Memmap.h"
#include "ConfigManager.h"

#include "OnScreenDisplay.h"
#define VSYNC_ENABLED 0

static volatile u32 s_swapRequested = false;

static volatile struct
{
	u32 xfbAddr;
	u32 fbWidth;
	u32 fbHeight;
} s_beginFieldArgs;

namespace SW
{

static volatile bool fifoStateRun = false;
static volatile bool emuRunningState = false;
static std::mutex m_csSWVidOccupied;

std::string VideoSoftware::GetName()
{
	return _trans("Software Renderer");
}

void *DllDebugger(void *_hParent, bool Show)
{
	return NULL;
}

void VideoSoftware::ShowConfig(void *_hParent)
{
#if defined(HAVE_WX) && HAVE_WX
	VideoConfigDialog diag((wxWindow*)_hParent, "Software", "gfx_software");
	diag.ShowModal();
#endif
}

bool VideoSoftware::Initialize(void *&window_handle)
{
	g_SWVideoConfig.Load((File::GetUserPath(D_CONFIG_IDX) + "gfx_software.ini").c_str());

	InitInterface();
	GLInterface->SetMode(GLInterfaceMode::MODE_DETECT);
	if (!GLInterface->Create(window_handle))
	{
		INFO_LOG(VIDEO, "%s", "SWRenderer::Create failed\n");
		return false;
	}

	InitBPMemory();
	InitXFMemory();
	SWCommandProcessor::Init();
	SWPixelEngine::Init();
	OpcodeDecoder::Init();
	Clipper::Init();
	Rasterizer::Init();
	HwRasterizer::Init();
	SWRenderer::Init();
	DebugUtil::Init();

	return true;
}

void VideoSoftware::DoState(PointerWrap& p)
{
	bool software = true;
	p.Do(software);
	if (p.GetMode() == PointerWrap::MODE_READ && software == false)
		// change mode to abort load of incompatible save state.
		p.SetMode(PointerWrap::MODE_VERIFY);

	// TODO: incomplete?
	SWCommandProcessor::DoState(p);
	SWPixelEngine::DoState(p);
	EfbInterface::DoState(p);
	OpcodeDecoder::DoState(p);
	Clipper::DoState(p);
	p.Do(swxfregs);
	p.Do(bpmem);
	p.DoPOD(swstats);

	// CP Memory
	p.DoArray(arraybases, 16);
	p.DoArray(arraystrides, 16);
	p.Do(MatrixIndexA);
	p.Do(MatrixIndexB);
	p.Do(g_VtxDesc.Hex);
	p.DoArray(g_VtxAttr, 8);
	p.DoMarker("CP Memory");

}

void VideoSoftware::CheckInvalidState()
{
	// there is no state to invalidate
}

void VideoSoftware::PauseAndLock(bool doLock, bool unpauseOnUnlock)
{
	if (doLock)
	{
		EmuStateChange(EMUSTATE_CHANGE_PAUSE);
		if (!Core::IsGPUThread())
			m_csSWVidOccupied.lock();
	}
	else
	{
		if (unpauseOnUnlock)
			EmuStateChange(EMUSTATE_CHANGE_PLAY);
		if (!Core::IsGPUThread())
			m_csSWVidOccupied.unlock();
	}
}

void VideoSoftware::RunLoop(bool enable)
{
	emuRunningState = enable;
}

void VideoSoftware::EmuStateChange(EMUSTATE_CHANGE newState)
{
	emuRunningState = (newState == EMUSTATE_CHANGE_PLAY) ? true : false;
}

void VideoSoftware::Shutdown()
{
	// TODO: should be in Video_Cleanup
	HwRasterizer::Shutdown();
	SWRenderer::Shutdown();

	// Do our OSD callbacks
	OSD::DoCallbacks(OSD::OSD_SHUTDOWN);

	GLInterface->Shutdown();
}

void VideoSoftware::Video_Cleanup()
{
}

// This is called after Video_Initialize() from the Core
void VideoSoftware::Video_Prepare()
{
	GLInterface->MakeCurrent();

	// Init extension support.
	if (!GLExtensions::Init())
	{
		ERROR_LOG(VIDEO, "GLExtensions::Init failed!Does your video card support OpenGL 2.0?");
		return;
	}

	// Handle VSync on/off
	GLInterface->SwapInterval(VSYNC_ENABLED);

	// Do our OSD callbacks
	OSD::DoCallbacks(OSD::OSD_INIT);

	HwRasterizer::Prepare();
	SWRenderer::Prepare();

	INFO_LOG(VIDEO, "Video backend initialized.");
}

// Run from the CPU thread (from VideoInterface.cpp)
void VideoSoftware::Video_BeginField(u32 xfbAddr, u32 fbWidth, u32 fbHeight)
{
	s_beginFieldArgs.xfbAddr = xfbAddr;
	s_beginFieldArgs.fbWidth = fbWidth;
	s_beginFieldArgs.fbHeight = fbHeight;
}

// Run from the CPU thread (from VideoInterface.cpp)
void VideoSoftware::Video_EndField()
{
	// Techincally the XFB is continually rendered out scanline by scanline between
	// BeginField and EndFeild, We could possibly get away with copying out the whole thing
	// at BeginField for less lag, but for the safest emulation we run it here.

	if (g_bSkipCurrentFrame || s_beginFieldArgs.xfbAddr == 0 ) {
		swstats.frameCount++;
		swstats.ResetFrame();
		Core::Callback_VideoCopiedToXFB(false);
		return;
	}
	if (!g_SWVideoConfig.bHwRasterizer) {
		if(!g_SWVideoConfig.bBypassXFB) {
			EfbInterface::yuv422_packed *xfb = (EfbInterface::yuv422_packed *) Memory::GetPointer(s_beginFieldArgs.xfbAddr);

			SWRenderer::UpdateColorTexture(xfb, s_beginFieldArgs.fbWidth, s_beginFieldArgs.fbHeight);
		}
	}

	// Idealy we would just move all the opengl contex stuff to the CPU thread, but this gets
	// messy when the Hardware Rasterizer is enabled.
	// And Neobrain loves his Hardware Rasterizer

	// If we are runing dual core, Signal the GPU thread about the new colour texture.
	if (SConfig::GetInstance().m_LocalCoreStartupParameter.bCPUThread)
		Common::AtomicStoreRelease(s_swapRequested, true);
	else
		SWRenderer::Swap(s_beginFieldArgs.fbWidth, s_beginFieldArgs.fbHeight);
}

u32 VideoSoftware::Video_AccessEFB(EFBAccessType type, u32 x, u32 y, u32 InputData)
{
	u32 value = 0;

	switch (type)
	{
	case PEEK_Z:
		{
			value = EfbInterface::GetDepth(x, y);
			break;
		}

	case POKE_Z:
		break;

	case PEEK_COLOR:
		{
			u32 color = 0;
			EfbInterface::GetColor(x, y, (u8*)&color);

			// rgba to argb
			value = (color >> 8) | (color & 0xff) << 24;
			break;
		}

	case POKE_COLOR:
		break;
	}

	return value;
}

u32 VideoSoftware::Video_GetQueryResult(PerfQueryType type)
{
	// TODO:
	return 0;
}

bool VideoSoftware::Video_Screenshot(const char *_szFilename)
{
	SWRenderer::SetScreenshot(_szFilename);
	return true;
}

// Run from the graphics thread
static void VideoFifo_CheckSwapRequest()
{
	if (Common::AtomicLoadAcquire(s_swapRequested))
	{
		SWRenderer::Swap(s_beginFieldArgs.fbWidth, s_beginFieldArgs.fbHeight);
		Common::AtomicStoreRelease(s_swapRequested, false);
	}
}

// -------------------------------
// Enter and exit the video loop
// -------------------------------
void VideoSoftware::Video_EnterLoop()
{
	std::lock_guard<std::mutex> lk(m_csSWVidOccupied);
	fifoStateRun = true;

	while (fifoStateRun)
	{
		VideoFifo_CheckSwapRequest();
		g_video_backend->PeekMessages();

		if (!SWCommandProcessor::RunBuffer())
		{
			Common::YieldCPU();
		}

		while (!emuRunningState && fifoStateRun)
		{
			g_video_backend->PeekMessages();
			VideoFifo_CheckSwapRequest();
			m_csSWVidOccupied.unlock();
			Common::SleepCurrentThread(1);
			m_csSWVidOccupied.lock();
		}
	}
}

void VideoSoftware::Video_ExitLoop()
{
	fifoStateRun = false;
}

// TODO : could use the OSD class in video common, we would need to implement the Renderer class
//        however most of it is useless for the SW backend so we could as well move it to its own class
void VideoSoftware::Video_AddMessage(const char* pstr, u32 milliseconds)
{
}
void VideoSoftware::Video_ClearMessages()
{
}

void VideoSoftware::Video_SetRendering(bool bEnabled)
{
	SWCommandProcessor::SetRendering(bEnabled);
}

void VideoSoftware::Video_GatherPipeBursted()
{
	SWCommandProcessor::GatherPipeBursted();
}

bool VideoSoftware::Video_IsPossibleWaitingSetDrawDone(void)
{
	return false;
}

bool VideoSoftware::Video_IsHiWatermarkActive(void)
{
	return false;
}


void VideoSoftware::Video_AbortFrame(void)
{
}

readFn16 VideoSoftware::Video_CPRead16()
{
	return SWCommandProcessor::Read16;
}
writeFn16 VideoSoftware::Video_CPWrite16()
{
	return SWCommandProcessor::Write16;
}

readFn16  VideoSoftware::Video_PERead16()
{
	return SWPixelEngine::Read16;
}
writeFn16 VideoSoftware::Video_PEWrite16()
{
	return SWPixelEngine::Write16;
}
writeFn32 VideoSoftware::Video_PEWrite32()
{
	return SWPixelEngine::Write32;
}


// Draw messages on top of the screen
unsigned int VideoSoftware::PeekMessages()
{
	return GLInterface->PeekMessages();
}

// Show the current FPS
void VideoSoftware::UpdateFPSDisplay(const char *text)
{
	char temp[100];
	snprintf(temp, sizeof temp, "%s | Software | %s", scm_rev_str, text);
	GLInterface->UpdateFPSDisplay(temp);
}

}
