// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "frame_trace.h"
#include "log.h"

#include <cstdarg>
#include <cstdlib>

namespace mwxr {

LONGLONG
QpcMicroseconds()
{
	static const LONGLONG frequency = [] {
		LARGE_INTEGER value;
		QueryPerformanceFrequency(&value);
		return value.QuadPart;
	}();
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return (LONGLONG)((double)counter.QuadPart * 1e6 / (double)frequency);
}

bool
FrameTrace::Open(ID3D11Device *device, ID3D11DeviceContext *context, int64_t xrTimeOffsetNs)
{
	const char *path = getenv("MWXR_DRIVER_FRAME_TRACE");
	if (!path || !path[0]) {
		return false;
	}
	file_ = fopen(path, "a");
	if (!file_) {
		Log("Frame trace: cannot open %s\n", path);
		return false;
	}
	setvbuf(file_, nullptr, _IOFBF, 65536);

	ID3D11Device5 *device5 = nullptr;
	if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device5), (void **)&device5)) &&
	    SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&context4_)) &&
	    SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), (void **)&fence_))) {
		event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
		waiter_ = std::thread([this] { WaitLoop(); });
	}
	if (device5) {
		device5->Release();
	}
	fprintf(file_, "# pid=%lu qpc_us_now=%lld xr_time_ns=qpc_us*1000%+lld gpu_rows=%d\n", GetCurrentProcessId(),
	        QpcMicroseconds(), (long long)xrTimeOffsetNs, fence_ != nullptr);
	fprintf(file_, "kind,frame,wait_begin,wait_end,predicted_display,period_us,should_render,begin_end,"
	               "present_begin,prepared,acquire_end,acquired,released,end_begin,end_end,layers\n");
	Log("Frame trace: %s (GPU rows %s)\n", path, fence_ ? "on" : "unavailable");
	return true;
}

FrameTrace::~FrameTrace()
{
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		stopping_ = true;
	}
	queueReady_.notify_all();
	if (waiter_.joinable()) {
		waiter_.join();
	}
	if (fence_) {
		fence_->Release();
	}
	if (context4_) {
		context4_->Release();
	}
	if (event_) {
		CloseHandle(event_);
	}
	if (file_) {
		fclose(file_);
	}
}

void
FrameTrace::SignalGpu(uint64_t frame, bool copies)
{
	if (!fence_) {
		return;
	}
	uint64_t value = frame * 2 + (copies ? 1 : 0);
	if (value <= lastValue_) {
		return; // the fence only moves forward
	}
	lastValue_ = value;
	context4_->Signal(fence_, value);
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		pending_.push_back(value);
	}
	queueReady_.notify_one();
}

void
FrameTrace::WaitLoop()
{
	for (;;) {
		uint64_t value;
		{
			std::unique_lock<std::mutex> lock(queueMutex_);
			queueReady_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
			if (stopping_) {
				return;
			}
			value = pending_.front();
			pending_.pop_front();
		}
		bool signalled = fence_->GetCompletedValue() >= value;
		if (!signalled && SUCCEEDED(fence_->SetEventOnCompletion(value, event_))) {
			signalled = WaitForSingleObject(event_, 2000) == WAIT_OBJECT_0;
		}
		Write("gpu,%llu,%s,%lld,%d\n", (unsigned long long)(value / 2), (value & 1) ? "copies" : "source",
		      QpcMicroseconds(), signalled ? 1 : 0);
	}
}

void
FrameTrace::Write(const char *format, ...)
{
	if (!file_) {
		return;
	}
	std::lock_guard<std::mutex> lock(fileMutex_);
	va_list args;
	va_start(args, format);
	vfprintf(file_, format, args);
	va_end(args);
	LONGLONG now = QpcMicroseconds();
	if (now - lastFlush_ >= 1000000) {
		fflush(file_);
		lastFlush_ = now;
	}
}

} // namespace mwxr
