// Hardware cursor of one monitor: Windows stops drawing the pointer into the monitor's image and hands its position
// and shape to the driver instead (Lumen draws the pointer on the client, without the video's latency).

#include "driver.h"

#include <avrt.h>

namespace lumen_vdd {

namespace {

// Large enough for the biggest pointer Windows offers (accessibility size 15 at 200 % scale).
constexpr UINT kMaxCursorSide = 256;

}  // namespace

CursorWatch::CursorWatch(IDDCX_MONITOR monitor, uint32_t id) : monitor_(monitor), id_(id) {
    shape_.resize(size_t{kMaxCursorSide} * kMaxCursorSide * 4);
    data_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (data_event_ == nullptr || stop_event_ == nullptr) {
        Log("monitor %u: cursor events: %lu", id_, GetLastError());
        return;
    }
    IDARG_IN_SETUP_HWCURSOR setup = {};
    setup.CursorInfo.Size = sizeof(setup.CursorInfo);
    setup.CursorInfo.AlphaCursorSupport = TRUE;
    setup.CursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
    setup.CursorInfo.MaxX = kMaxCursorSide;
    setup.CursorInfo.MaxY = kMaxCursorSide;
    setup.hNewCursorDataAvailable = data_event_;
    const NTSTATUS status = IddCxMonitorSetupHardwareCursor(monitor_, &setup);
    if (!NT_SUCCESS(status)) {
        // Windows keeps drawing the pointer into the image (software cursor).
        Log("monitor %u: IddCxMonitorSetupHardwareCursor failed: 0x%08lx", id_, status);
        return;
    }
    thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (thread_ == nullptr) {
        Log("monitor %u: cursor thread: %lu", id_, GetLastError());
    }
}

CursorWatch::~CursorWatch() {
    if (thread_ != nullptr) {
        SetEvent(stop_event_);
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
    }
    if (data_event_ != nullptr) {
        CloseHandle(data_event_);
    }
    if (stop_event_ != nullptr) {
        CloseHandle(stop_event_);
    }
}

DWORD CALLBACK CursorWatch::ThreadProc(void* param) {
    auto* self = static_cast<CursorWatch*>(param);
    DWORD task_index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Distribution", &task_index);
    self->Watch();
    if (task != nullptr) {
        AvRevertMmThreadCharacteristics(task);
    }
    return 0;
}

void CursorWatch::Watch() {
    for (;;) {
        HANDLE events[] = {data_event_, stop_event_};
        if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) != WAIT_OBJECT_0) {
            return;
        }
        IDARG_IN_QUERY_HWCURSOR in = {};
        IDARG_OUT_QUERY_HWCURSOR out = {};
        {
            std::lock_guard<std::mutex> guard(lock_);
            in.LastShapeId = state_.shape_id;
            in.ShapeBufferSizeInBytes = static_cast<UINT>(shape_.size());
            in.pShapeBuffer = shape_.data();
            const NTSTATUS status = IddCxMonitorQueryHardwareCursor(monitor_, &in, &out);
            if (!NT_SUCCESS(status)) {
                Log("monitor %u: IddCxMonitorQueryHardwareCursor failed: 0x%08lx", id_, status);
                continue;
            }
            state_.visible = out.IsCursorVisible != FALSE;
            state_.x = out.X;
            state_.y = out.Y;
            state_.updates++;
            if (out.IsCursorShapeUpdated) {
                const auto& s = out.CursorShapeInfo;
                state_.shape_id = s.ShapeId;
                state_.alpha = s.CursorType == IDDCX_CURSOR_SHAPE_TYPE_ALPHA;
                state_.width = s.Width;
                state_.height = s.Height;
                state_.pitch = s.Pitch;
                state_.hot_x = s.XHot;
                state_.hot_y = s.YHot;
                Log("monitor %u: cursor shape %u, %s %ux%u, hotspot (%u, %u)", id_, s.ShapeId,
                    state_.alpha ? "alpha" : "masked color", s.Width, s.Height, s.XHot, s.YHot);
            }
        }
    }
}

}  // namespace lumen_vdd
