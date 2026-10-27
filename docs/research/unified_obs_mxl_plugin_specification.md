# Component Specification: Unified OBS MXL Plugin (`obs-mxl`)

## 1. Executive Summary & Purpose

The **Unified OBS MXL Plugin (`obs-mxl`)** is a native in-process C++ module for OBS Studio. It consolidates both uncompressed ingress (media sources) and uncompressed egress (program and preview outputs) into a single installable plugin (`obs-mxl.so`).

By implementing both `obs_source_info` and `obs_output_info` within the same library, the plugin allows OBS Studio to function as an uncompressed visual switcher and compositor inside an EBU Dynamic Media Facility (DMF) environment without requiring separate binaries or redundant background services.

Following the UX pattern established by **Distro-AV (formerly obs-ndi)**:
* **Ingress** is exposed as standard OBS Sources (`MXL Source`) added via the Sources dock.
* **Egress** avoids persistent UI dock widgets entirely; instead, it is configured and toggled via a clean dialog under **Tools $\rightarrow$ MXL Output Settings**, allowing independent broadcast activation of the Master Program and Preview feeds into POSIX shared memory (`/dev/shm/mxl`).

---

## 2. Technical Stack & Dependencies

* **Language:** C++20 / C11.
* **Host Application:** OBS Studio (version 30.x+ / 31.x+ `libobs` API).
* **UI Framework:** Qt 6 (or Qt 5 depending on OBS build target) via `obs-frontend-api`.
* **Shared Memory Interprocess Communication (IPC):** EBU MXL C SDK (`libmxl`) v1.1.
* **OS / Kernel:** Linux with POSIX shared memory enabled (`tmpfs` mounted at `/dev/shm`).
* **Clock Reference:** Linux kernel clock `CLOCK_TAI` (disciplined by `chrony` NTP / PTP).

---

## 3. Unified Plugin Architecture

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│ OBS Studio Process Space (obs-mxl.so)                                            │
│                                                                                  │
│  [ Module Entry: obs_module_load() ]                                             │
│    ├── obs_register_source(&mxl_source_info) ──> Ingress Pipeline                │
│    ├── obs_register_output(&mxl_output_info) ──> Egress Pipeline                 │
│    └── obs_frontend_add_tools_menu_qaction() ──> "Tools -> MXL Output Settings"  │
│                                                                                  │
│  ┌─────────────────────────┐             ┌────────────────────────────────────┐  │
│  │ Ingress: MXL Source     │             │ Egress: MXL Output                 │  │
│  │ (obs_source_info)       │             │ (obs_output_info)                  │  │
│  │                         │             │                                    │  │
│  │ • Reader Thread         │             │ • Raw Canvas Callback              │  │
│  │ • Reads /dev/shm/mxl    │             │ • Master Audio Callback            │  │
│  │ • Converts TAI -> Monot.│             │ • Grid Quantization (CLOCK_TAI)    │  │
│  │ • obs_source_output_*   │             │ • Commits to /dev/shm/mxl          │  │
│  └───────────▲─────────────┘             └─────────────────┬──────────────────┘  │
└──────────────┼─────────────────────────────────────────────┼─────────────────────┘
               │                                             │
       (Zero-Copy Read)                              (Zero-Copy Write)
               │                                             │
┌──────────────┴─────────────────────────────────────────────▼─────────────────────┐
│ POSIX Shared Memory (/dev/shm/mxl/)                                              │
│  ├── <camera_flow_uuid>/   (DeckLink Ingest Daemon)                              │
│  ├── <graphics_flow_uuid>/ (NDI Ingest Daemon)                                   │
│  ├── <obs_pgm_flow_uuid>/  (OBS Program Egress -> FFmpeg, AI Tracking, Signage)  │
│  └── <obs_prev_flow_uuid>/ (OBS Preview Egress -> Multiviewer / Secondary Ops)  │
└──────────────────────────────────────────────────────────────────────────────────┘
```

---

## 4. Module Lifecycle & Registration

The plugin exports standard OBS module lifecycle functions.

```cpp
#include <obs/obs-module.h>
#include <obs-frontend-api.h>
#include "mxl-source.h"
#include "mxl-output.h"
#include "ui/mxl-output-dialog.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-mxl", "en-US")

static QAction *tools_menu_action = nullptr;
static MxlOutputDialog *output_dialog = nullptr;

bool obs_module_load(void) {
    blog(LOG_INFO, "[obs-mxl] Initializing Unified MXL Plugin v1.1.0");

    // 1. Register Ingest Source
    obs_register_source(&mxl_source_info);

    // 2. Register Egress Output Endpoint
    obs_register_output(&mxl_output_info);

    // 3. Register "Tools -> MXL Output Settings" Action via Frontend API
    tools_menu_action = (QAction *)obs_frontend_add_tools_menu_qaction("MXL Output Settings");
    
    auto menu_cb = []() {
        if (!output_dialog) {
            QMainWindow *main_window = (QMainWindow *)obs_frontend_get_main_window();
            output_dialog = new MxlOutputDialog(main_window);
        }
        output_dialog->show();
        output_dialog->raise();
        output_dialog->activateWindow();
    };

    QObject::connect(tools_menu_action, &QAction::triggered, menu_cb);

    // 4. Hook Frontend Events for Profile Persistence and Clean Shutdown
    obs_frontend_add_event_callback([](enum obs_frontend_event event, void *private_data) {
        if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
            MxlOutputDialog::AutoStartOutputsIfNeeded();
        } else if (event == OBS_FRONTEND_EVENT_EXIT) {
            MxlOutputDialog::StopAllOutputs();
        }
    }, nullptr);

    return true;
}

void obs_module_unload(void) {
    if (output_dialog) {
        delete output_dialog;
        output_dialog = nullptr;
    }
    blog(LOG_INFO, "[obs-mxl] Plugin unloaded successfully");
}
```

---

## 5. Ingress Subsystem: `MXL Source` (`obs_source_info`)

The source component discovers and reads raw grains from `/dev/shm/mxl/<flow_id>` without network transport overhead.

### 5.1. Source Interface Definition
```cpp
static struct obs_source_info mxl_source_info = {
    .id             = "mxl_input_source",
    .type           = OBS_SOURCE_TYPE_INPUT,
    .output_flags   = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name       = [](void *) { return "MXL Source"; },
    .create         = mxl_source_create,
    .destroy        = mxl_source_destroy,
    .get_defaults   = mxl_source_get_defaults,
    .get_properties = mxl_source_get_properties,
    .update         = mxl_source_update,
    .show           = mxl_source_show,
    .hide           = mxl_source_hide,
};
```

### 5.2. Reader Worker Implementation
The worker thread executes a playout delay loop, quantizes reader queries to the host's SMPTE 2059-1 TAI timeline, and normalizes timestamps into OBS monotonic time (`os_gettime_ns()`):

```cpp
void* MxlSourceReaderThread(void *data) {
    auto *context = static_cast<MxlSourceContext*>(data);

    // Calibrate TAI to OBS monotonic timeline offset
    struct timespec ts_tai;
    clock_gettime(CLOCK_TAI, &ts_tai);
    uint64_t tai_now = (uint64_t)ts_tai.tv_sec * 1000000000ULL + ts_tai.tv_nsec;
    context->time_offset_ns = (int64_t)os_gettime_ns() - (int64_t)tai_now;

    const uint64_t delay_ns = (uint64_t)context->playout_delay_ms * 1000000ULL;
    mxlFlowMetadata meta = mxl_reader_get_metadata(context->video_reader);
    const uint64_t grain_duration = meta.video.grain_duration_ns;

    while (context->running) {
        clock_gettime(CLOCK_TAI, &ts_tai);
        uint64_t current_tai = (uint64_t)ts_tai.tv_sec * 1000000000ULL + ts_tai.tv_nsec;
        uint64_t target_tai = (current_tai > delay_ns) ? (current_tai - delay_ns) : 0;
        uint64_t target_index = target_tai / grain_duration;

        if (target_index <= context->last_grain_index) {
            uint64_t next_frame_tai = (context->last_grain_index + 1) * grain_duration + delay_ns;
            if (next_frame_tai > current_tai) {
                os_sleepto_ns(os_gettime_ns() + (next_frame_tai - current_tai));
            }
            continue;
        }

        // Pull video grain with 25ms timeout
        mxlGrainInfo grain_info;
        const uint8_t *video_data = nullptr;
        mxlStatus status = mxl_flow_reader_get_grain(
            context->video_reader, target_index, 25000000, &grain_info, &video_data
        );

        if (status == MXL_STATUS_SUCCESS) {
            struct obs_source_frame2 obs_frame = {};
            obs_frame.format = MapMxlFormatToObs(meta.video.pixel_format);
            obs_frame.width = meta.video.width;
            obs_frame.height = meta.video.height;
            obs_frame.data[0] = const_cast<uint8_t*>(video_data);
            obs_frame.linesize[0] = meta.video.stride_bytes;
            obs_frame.timestamp = grain_info.sync_timestamp + context->time_offset_ns;

            obs_source_output_video2(context->source, &obs_frame);
            context->last_grain_index = target_index;
        } else if (status == MXL_STATUS_OVERRUN) {
            context->last_grain_index = mxl_reader_get_head_index(context->video_reader) - 1;
        }

        // Pull audio samples for the corresponding time window
        if (context->audio_reader) {
            PullAudioSlice(context, target_tai, grain_duration);
        }
    }
    return nullptr;
}
```

---

## 6. Egress Subsystem: `MXL Output` (`obs_output_info`)

The egress component registers as an unencoded media output (`OBS_OUTPUT_VIDEO | OBS_OUTPUT_AUDIO`), capturing post-compositor program rasters and the master audio mix directly from GPU/CPU buffers.

### 6.1. Output Interface Definition
```cpp
static struct obs_output_info mxl_output_info = {
    .id             = "mxl_output",
    .flags          = OBS_OUTPUT_VIDEO | OBS_OUTPUT_AUDIO,
    .get_name       = [](void *) { return "MXL Egress Output"; },
    .create         = mxl_output_create,
    .destroy        = mxl_output_destroy,
    .start          = mxl_output_start,
    .stop           = mxl_output_stop,
    .raw_video      = mxl_output_raw_video,
    .raw_audio      = mxl_output_raw_audio,
    .get_properties = mxl_output_get_properties,
};
```

### 6.2. Frame Capture & Ring Buffer Commit
```cpp
static void mxl_output_raw_video(void *data, struct video_data *frame) {
    auto *context = static_cast<MxlOutputContext*>(data);

    // 1. Capture completion timestamp in host CLOCK_TAI
    struct timespec ts;
    clock_gettime(CLOCK_TAI, &ts);
    uint64_t now_tai = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

    // 2. Quantize to facility grid
    uint64_t grain_index = now_tai / context->grain_duration_ns;
    if (grain_index <= context->last_committed_index) {
        grain_index = context->last_committed_index + 1;
    }

    uint32_t slot = grain_index % context->ring_buffer_depth;
    uint8_t *dest_plane = context->video_slots[slot];

    // 3. Memory copy from OBS raw buffer into /dev/shm
    // For packed formats (e.g., UYVY or RGBA), plane 0 is contiguous
    memcpy(dest_plane, frame->data[0], frame->linesize[0] * context->height);

    // 4. Update slot metadata and signal downstream consumers
    context->slot_headers[slot].sync_timestamp = grain_index * context->grain_duration_ns;
    context->slot_headers[slot].origin_timestamp = now_tai;
    
    atomic_store_explicit(&context->shared_header->head_grain_index, grain_index, memory_order_release);
    mxl_futex_wake(&context->shared_header->futex_signal);

    context->last_committed_index = grain_index;
}

static void mxl_output_raw_audio(void *data, struct audio_data *frame) {
    auto *context = static_cast<MxlOutputContext*>(data);
    
    struct timespec ts;
    clock_gettime(CLOCK_TAI, &ts);
    uint64_t now_tai = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

    // Calculate sample index on 48kHz timeline
    uint64_t sample_index = (now_tai * 48000ULL) / 1000000000ULL;

    // Write de-interleaved float32 channels into continuous ring buffer
    for (size_t c = 0; c < context->audio_channels; ++c) {
        mxl_audio_writer_commit_channel(
            context->audio_writer, c, sample_index, 
            (const float*)frame->data[c], frame->frames
        );
    }
}
```

---

## 7. Distro-AV Style UI Integration (Tools Menu)

Rather than cluttering the screen with a persistent dock widget, the plugin implements a clean settings dialog accessible via **Tools $\rightarrow$ MXL Output Settings**.

### 7.1. Dialog Layout (`ui/mxl-output-dialog.ui`)
The dialog provides configuration for both **Main (Program) Output** and **Preview Output**:

```
┌─────────────────────────────────────────────────────────────┐
│ MXL Output Settings                                    [ X ]│
├─────────────────────────────────────────────────────────────┤
│  Shared Memory Domain: [/dev/shm/mxl                      ] │
│                                                             │
│  ┌─ Main Output (Program) ────────────────────────────────┐ │
│  │ [X] Enable Main Output                                 │ │
│  │ Flow Name: [OBS_PGM_Live                             ] │ │
│  │ Flow UUID: [3fa85f64-5717-4562-b3fc-2c963f66afa6    ] │ │
│  │ Ring Buffer Depth: [ 8 ] frames                        │ │
│  │ Status: ● ACTIVE (1920x1080 @ 59.94p | 8 Ch Float32)   │ │
│  └────────────────────────────────────────────────────────┘ │
│                                                             │
│  ┌─ Preview Output ───────────────────────────────────────┐ │
│  │ [ ] Enable Preview Output                              │ │
│  │ Flow Name: [OBS_Preview                              ] │ │
│  │ Flow UUID: [7c9e6679-7425-40de-944b-e07fc1f90ae7    ] │ │
│  │ Ring Buffer Depth: [ 8 ] frames                        │ │
│  │ Status: ○ INACTIVE                                     │ │
│  └────────────────────────────────────────────────────────┘ │
│                                                             │
│                                      [ Apply ]  [ Close ]   │
└─────────────────────────────────────────────────────────────┘
```

### 7.2. Controller Implementation (`ui/mxl-output-dialog.cpp`)

The controller manages the lifecycle of `obs_output_t` instances independently of the UI display state:

```cpp
#include "mxl-output-dialog.h"
#include <obs-frontend-api.h>

MxlOutputDialog::MxlOutputDialog(QWidget *parent) : QDialog(parent) {
    ui.setupUi(this);
    LoadSettings();
}

void MxlOutputDialog::OnApplyClicked() {
    SaveSettings();

    // Configure and toggle Main (Program) Output
    if (ui.mainOutputEnabled->isChecked()) {
        if (!main_output_) {
            obs_data_t *settings = obs_data_create();
            obs_data_set_string(settings, "domain_path", ui.domainPath->text().toUtf8().constData());
            obs_data_set_string(settings, "flow_name", ui.mainFlowName->text().toUtf8().constData());
            obs_data_set_string(settings, "flow_uuid", ui.mainFlowUuid->text().toUtf8().constData());
            obs_data_set_int(settings, "buffer_depth", ui.mainBufferDepth->value());

            main_output_ = obs_output_create("mxl_output", "MXL Program Output", settings, nullptr);
            obs_data_release(settings);

            // Bind to Program video/audio mix
            obs_output_set_media(main_output_, obs_get_video(), obs_get_audio());
            obs_output_start(main_output_);
        }
    } else {
        if (main_output_) {
            obs_output_stop(main_output_);
            obs_output_release(main_output_);
            main_output_ = nullptr;
        }
    }

    // Configure and toggle Preview Output
    if (ui.previewOutputEnabled->isChecked()) {
        if (!preview_output_) {
            obs_data_t *settings = obs_data_create();
            obs_data_set_string(settings, "domain_path", ui.domainPath->text().toUtf8().constData());
            obs_data_set_string(settings, "flow_name", ui.previewFlowName->text().toUtf8().constData());
            obs_data_set_string(settings, "flow_uuid", ui.previewFlowUuid->text().toUtf8().constData());
            obs_data_set_int(settings, "buffer_depth", ui.previewBufferDepth->value());

            preview_output_ = obs_output_create("mxl_output", "MXL Preview Output", settings, nullptr);
            obs_data_release(settings);

            // Bind to Preview video canvas
            video_t *preview_video = obs_frontend_get_rendering_video();
            obs_output_set_media(preview_output_, preview_video, obs_get_audio());
            obs_output_start(preview_output_);
        }
    } else {
        if (preview_output_) {
            obs_output_stop(preview_output_);
            obs_output_release(preview_output_);
            preview_output_ = nullptr;
        }
    }
}
```

### 7.3. Auto-Start & Session Persistence
Configuration settings are stored inside OBS's active profile using `obs_frontend_get_profile_config()`:
* When OBS launches, an event listener for `OBS_FRONTEND_EVENT_FINISHED_LOADING` inspects saved preferences.
* If `main_output_enabled` was active during the previous session, the plugin initializes and starts the MXL output automatically without user intervention.
* When OBS exits (`OBS_FRONTEND_EVENT_EXIT`), all active outputs are cleanly signaled to stop and memory maps are detached.

---

## 8. Memory Management & Threading Safety

1. **Single-Writer Multiple-Reader (SWMR):**
   * The plugin guarantees that only one process writes to an egress flow (`head_grain_index` is modified strictly by `obs_output_raw_video`).
   * When functioning as a source, the plugin opens all flow memory files in read-only mode (`O_RDONLY`, `PROT_READ`).
2. **Futex-Based Reader Notification:**
   * Downstream processes (such as FFmpeg or the AI auto-framer) sleep using Linux userspace futexes (`sys_futex`).
   * The plugin avoids busy-polling or thread-yielding loops on egress, issuing a futex wake call only upon completing a full raster write.
3. **Zero-Copy Ingress Passing:**
   * In `MxlSourceReaderThread`, the memory-mapped pointer from `/dev/shm` is assigned directly to `obs_source_frame2.data[0]`.
   * Video frame memory is not duplicated into temporary CPU buffers; OBS uploads pixels directly from `/dev/shm` into GPU VRAM textures via its graphics rendering pipeline.

---

## 9. Failure Modes & Edge Case Recovery

* **Downstream Reader Stall:** If external consumers (FFmpeg or the vertical AI service) hang or process frames slowly, the OBS output engine **never blocks**. The ring buffer overwrites historical slots according to its modulo depth formula ($GrainIndex \pmod 8$), guaranteeing that OBS render threads are never delayed.
* **Upstream Ingest Disappearance:** If an external SDI or NDI ingest worker terminates abnormally, the `MXL Source` reader times out on its futex wait. After three consecutive dropped frame intervals ($50\text{ ms}$ at 60p), the source issues a blank transparent frame (`alpha = 0x00`) to prevent the OBS compositor from freezing on a stale texture.
* **Dynamic Geometry Changes:** If an upstream ingest worker alters resolution or frame rate mid-stream, the source detects the incremented `metadata_generation` counter in `/dev/shm/mxl/<flow_id>/metadata`, reinitializes its internal buffer dimensions, and outputs the updated geometry on the next tick.