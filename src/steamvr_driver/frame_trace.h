// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// Opt-in per-frame trace of the driver's OpenXR frame loop, for joining with
// the OpenVR shim's trace and Monado's app_pacing timing trace.
//
// MWXR_DRIVER_FRAME_TRACE names the CSV file. Times are QueryPerformanceCounter
// microseconds, as in the shim's trace; the header gives the offset to the
// runtime's XrTime. Rows:
//   frame: CPU stages of one OpenXR frame (0 where a stage did not run)
//   gpu:   when a fence signalled after the source's keyed-mutex GPU wait
//          (which=source) or after the copies (which=copies)
//   vsync: each VsyncEvent sent to SteamVR
// The column header describes frame rows; gpu rows are
// kind,frame,which,qpc_us,signalled and vsync rows are
// kind,qpc_us,vsync_offset_us,photons_us. GPU rows come from a waiting
// thread, which adds listener latency, and the signals wait for the
// context's next flush like any other GPU work.
#pragma once

#include <d3d11_4.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace mwxr {

LONGLONG
QpcMicroseconds();

class FrameTrace
{
public:
	~FrameTrace();

	// Opens the file when MWXR_DRIVER_FRAME_TRACE is set. GPU rows need
	// ID3D11Device5 fences; without them only CPU rows are written.
	bool
	Open(ID3D11Device *device, ID3D11DeviceContext *context, int64_t xrTimeOffsetNs);

	bool
	Enabled() const
	{
		return file_ != nullptr;
	}

	// Queues a fence signal on the context (caller holds the context).
	void
	SignalGpu(uint64_t frame, bool copies);

	void
	Write(const char *format, ...);

private:
	void
	WaitLoop();

	std::mutex fileMutex_;
	FILE *file_ = nullptr;
	LONGLONG lastFlush_ = 0;

	ID3D11DeviceContext4 *context4_ = nullptr;
	ID3D11Fence *fence_ = nullptr;
	HANDLE event_ = nullptr;
	std::mutex queueMutex_;
	std::condition_variable queueReady_;
	std::deque<uint64_t> pending_; // fence values, frame * 2 + copies
	uint64_t lastValue_ = 0;
	bool stopping_ = false;
	std::thread waiter_;
};

} // namespace mwxr
