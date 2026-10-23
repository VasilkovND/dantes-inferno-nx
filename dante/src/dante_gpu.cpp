// dantes-inferno-nx - the game's graphics system on the Switch.
//
// Copied from halo3/src/halo3_gpu.cpp: nothing in it is about Halo 3. Once a second game uses it, it belongs in sdk/.
//
// What it does
//   Stands in for the Xbox 360 GPU, so the game runs past its first frame. The game's own Direct3D keeps
//   running unchanged and writes its list of GPU commands (the PM4 ring). A thread reads that list and does
//   what the game waits for:
//     1. MMIO registers at 0x7FC80000, with the fixed values the SDK's emulation returns;
//     2. the ring read pointer, written back where the game watches it (it spins on it to know how much
//        room is left);
//     3. interrupts: vblank, and PM4_INTERRUPT packets (the GPU reached that point of the ring);
//     4. GPU writes to memory (MEM_WRITE, COND_WRITE, REG_TO_MEM, EVENT_WRITE_*, scratch registers), as in
//        the SDK's graphics/command_processor.cpp. Occlusion queries report 1000 samples, like the SDK.
//   5. (deko3d build) the minimal renderer: IM_LOAD / IM_LOAD_IMMEDIATE tell which translated shader is loaded
//      (dante_shaders.h), and each draw goes to the SDK's deko3d backend (rex::x360::RenderDk) as a PeticionDibujo
//      with the register mirror; resolves and clears go to its Copiar, and each Swap presents the frontbuffer
//      that fetch constant 0 describes. A draw whose VS or PS is not identified is skipped and counted.
//
// Presentation (the deko3d build): the SDK's deko3d provider and presenter (rex/ui/deko3d/presentador.h). ReXApp
// sees them and sets up the window and the ImGui overlay as usual; until the renderer refreshes the guest output,
// the presenter paints black under the overlay.
//
// Without it the runtime drops the game's Vd* calls ("no GPU emulation loaded") and the render thread waits
// forever for a GPU that never reads its commands.

#include "dante_gpu.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/types.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>

#if DANTE_DEKO3D
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <string>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/ui/deko3d/presentador.h>
#include <rex/x360/decodificador.h>
#include <rex/x360/deko/destinos.h>
#include <rex/x360/destinos.h>
#include <rex/x360/peticion.h>

#include "dante_shaders.h"

// xxHash reads with memcpy under GCC's strict aliasing (see shaders/ds_shader_library.cpp).
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

// The SDK renderer's shadow-pass shortcuts (sdk/src/x360/deko/destinos.cpp; the SDK keeps their nfsmw_ names): both
// drop colorless draws that alpha-test or discard, which here are ordinary depth-only draws. Off for Dead Space 2.
REXCVAR_DECLARE(bool, nfsmw_sombras_sin_vegetacion);
REXCVAR_DECLARE(bool, nfsmw_nativo_ps_solo_alfa);
// The renderer's shadow-map depth bias, given when the game sets none: Dead Space 2's shadow maps are 960 wide.
REXCVAR_DECLARE(int32_t, nfsmw_dk_sombras_sesgo_pitch);
// The renderer's sampled change checks of geometry and textures stay at the SDK's defaults (sampled). Full fingerprints
// were tried against the streaks (which were the vertex index and the computed-index fetches, not stale vertices):
// they hashed 14-20 MB of vertices a frame and the ring thread went from 9 % to 40 % of a core.
REXCVAR_DECLARE(bool, nfsmw_dk_geometria_muestreo);
REXCVAR_DECLARE(int32_t, nfsmw_dk_perfil_gpu_s);  // the renderer's periodic GPU profile: off (each was a hitch)
#endif

namespace dante::gpu {
namespace {

namespace xenos = rex::graphics::xenos;
using rex::X_STATUS;  // X_STATUS_SUCCESS and X_STATUS_UNSUCCESSFUL need it too
using Clock = std::chrono::steady_clock;

constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x0000FFFF;
constexpr uint32_t kRegisterCount = 0x5003;  // RegisterFile::kRegisterCount

constexpr uint32_t kRegCpRbWptr = rex::graphics::XE_GPU_REG_CP_RB_WPTR;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;

constexpr int kMaxIndirectDepth = 4;
// The real GPU waits as long as it takes; without one some conditions are never met, so the wait is cut
// short and the ring goes on.
constexpr auto kWaitRegMemMax = std::chrono::milliseconds(200);
constexpr uint16_t kMaxExtent = 2048 >> 3;  // xenos::kTexture2DCubeMaxWidthHeight >> 3
constexpr int32_t kFakeSampleCount = 1000;  // the SDK's query_occlusion_fake_sample_count
constexpr auto kReportEvery = std::chrono::seconds(10);

// Reads the ring (a power-of-two buffer of words that wraps) or an indirect buffer (linear).
struct Reader {
  const uint8_t* base = nullptr;
  uint32_t mask = 0;  // words - 1 in the ring; 0 in a linear buffer
  uint32_t pos = 0;
  uint32_t end = 0;

  uint32_t Pending() const { return mask ? ((end - pos) & mask) : (end - pos); }
  uint32_t Peek() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Read() {
    const uint32_t value = Peek();
    Skip(1);
    return value;
  }
  void Skip(uint32_t words) { pos = mask ? ((pos + words) & mask) : (pos + words); }
};

bool Compare(uint32_t info, uint32_t value, uint32_t reference) {
  switch (info & 0x7) {
    case 0x0: return false;
    case 0x1: return value < reference;
    case 0x2: return value <= reference;
    case 0x3: return value == reference;
    case 0x4: return value != reference;
    case 0x5: return value >= reference;
    case 0x6: return value > reference;
    default: return true;
  }
}

// Register base of each SET_CONSTANT / LOAD_ALU_CONSTANT type, or UINT32_MAX.
uint32_t ConstantBase(uint32_t type_index) {
  switch ((type_index >> 16) & 0xFF) {
    case 0: return 0x4000;  // ALU
    case 1: return 0x4800;  // fetch
    case 2: return 0x4900;  // bool
    case 3: return 0x4908;  // loop
    case 4: return 0x2000;  // registers
    default: return UINT32_MAX;
  }
}

class RingGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  RingGraphicsSystem() : registers_(kRegisterCount, 0) {}
  ~RingGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
#if DANTE_DEKO3D
    if (presenter_) {
      return X_STATUS_SUCCESS;
    }
    app_context_ = app_context;
    if (!provider_) {
      provider_ = rex::deko3d::ProveedorDeko3d::Crear();  // a deko3d failure ends the process, logged
      if (!provider_) {
        REXLOG_ERROR("[gpu] could not create the deko3d device");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[gpu] could not create the deko3d presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[gpu] deko3d presenter up (the game's frames come with the renderer)");
#else
    (void)app_context;
#endif
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!mmio_registered_) {
      mmio_registered_ =
          memory_->AddVirtualMappedRange(kMmioBase, kMmioMask, kMmioSize, this, &ReadMmio, &WriteMmio);
      if (!mmio_registered_) {
        REXLOG_ERROR("[gpu] could not register the GPU's MMIO range");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (active_.exchange(true)) {
      return X_STATUS_SUCCESS;
    }
#if DANTE_DEKO3D
    REXCVAR_SET(nfsmw_sombras_sin_vegetacion, false);
    REXCVAR_SET(nfsmw_nativo_ps_solo_alfa, false);
    REXCVAR_SET(nfsmw_dk_sombras_sesgo_pitch, 960);
    {
      // No CPU stack sampling (a flag an earlier debug build may have left: it stops every thread each millisecond),
      // and no GPU profile of a frame every 20 s (a barrier and two timestamps around each draw): both were hitches.
      std::error_code ec;
      std::filesystem::remove(rex::filesystem::GetExecutableFolder() / "logs" / "rex" / "perfil_pilas.flag", ec);
      REXCVAR_SET(nfsmw_dk_perfil_gpu_s, 0);
    }
    // Before the game creates its shaders (the create hooks look the containers up in it).
    dante::shaders::Global().Cargar(rex::filesystem::GetExecutableFolder() / "dante_shaders.dslib");
#endif
    last_report_ = Clock::now();
    vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return VblankLoop(); }));
    vblank_thread_->set_name("GPU vblank");
    vblank_thread_->Create();
    ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return RingLoop(); }));
    ring_thread_->set_name("GPU ring");
    ring_thread_->Create();
    REXLOG_INFO("[gpu] ring reader started");
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[gpu] interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    // As in CommandProcessor::InitializeRingBuffer: (1 << (size_log2 + 3)) bytes.
    ring_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    ring_base_.store(ptr, std::memory_order_release);
    ring_generation_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[gpu] ring at {:08X}, {} words", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    read_pointer_writeback_.store(ptr, std::memory_order_release);
  }

  void Shutdown() override {
    if (!active_.exchange(false)) {
      DestroyPresentation();
      return;
    }
    {
      std::lock_guard<std::mutex> lock(ring_mutex_);
    }
    ring_cv_.notify_all();
    if (ring_thread_) {
      ring_thread_->Wait(0, 0, 0, nullptr);
      ring_thread_.reset();
    }
    if (vblank_thread_) {
      vblank_thread_->Wait(0, 0, 0, nullptr);
      vblank_thread_.reset();
    }
    DestroyPresentation();
  }

  // After the threads: the presenter goes before its provider, on the UI thread.
  void DestroyPresentation() {
    destinos_.reset();  // frees its deko3d memory before the device goes
    if (presenter_) {
      if (app_context_) {
        app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      }
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  // --- MMIO registers ---------------------------------------------------------

  static uint32_t ReadMmio(void* ppc_context, void* context, uint32_t address) {
    (void)ppc_context;
    return static_cast<RingGraphicsSystem*>(context)->ReadMmioRegister(address);
  }

  static void WriteMmio(void* ppc_context, void* context, uint32_t address, uint32_t value) {
    (void)ppc_context;
    static_cast<RingGraphicsSystem*>(context)->WriteMmioRegister(address, value);
  }

  // The same fixed values as the SDK's emulation (graphics/graphics_system.cpp).
  uint32_t ReadMmioRegister(uint32_t address) {
    const uint32_t r = (address & 0xFFFF) / 4;
    switch (r) {
      case kRegRbEdramTiming:
        return 0x08100748;
      case kRegRbBcControl:
        return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus:
        return 1;  // in vblank
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        const uint32_t width = std::min(uint32_t(mode.display_width), uint32_t(0x0FFF));
        const uint32_t height = std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
        return (width << 16) | height;
      }
      default:
        return Register(r);
    }
  }

  void WriteMmioRegister(uint32_t address, uint32_t value) {
    const uint32_t r = (address & 0xFFFF) / 4;
    if (r < kRegisterCount) {
      registers_[r] = value;  // no side effects through MMIO (graphics_system.cpp)
    }
    if (r == kRegCpRbWptr) {
      write_pointer_.store(value, std::memory_order_release);
      // Taking the lock for an instant avoids losing the wakeup if the ring thread is between checking
      // and going to sleep.
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_one();
    }
  }

  uint32_t Register(uint32_t index) const { return index < kRegisterCount ? registers_[index] : 0; }

  // Register writes from the ring, with the side effects the game relies on.
  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) {
      return;
    }
    if (index == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) {
      value |= UINT32_C(0x80000000);
    }
#if DANTE_DEKO3D
    // What the renderer caches per generation (PeticionDibujo): bumped only when a value really changes.
    if (registers_[index] != value) {
      if (index >= 0x4000 && index < 0x4800) {
        ++(index < 0x4400 ? generacion_constantes_vs_ : generacion_constantes_ps_);
      } else if (index >= 0x4800 && index <= 0x48BF) {
        ++generacion_fetch_;
      } else if (EsRegistroDeEncuadre(index)) {
        ++generacion_encuadre_;
      }
    }
#endif
    registers_[index] = value;
    // Scratch registers are mirrored to memory (SCRATCH_ADDR) for the ones SCRATCH_UMSK selects: the game
    // waits on those values to know where the GPU is.
    if (index >= rex::graphics::XE_GPU_REG_SCRATCH_REG0 && index <= rex::graphics::XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t n = index - rex::graphics::XE_GPU_REG_SCRATCH_REG0;
      if ((UINT32_C(1) << n) & registers_[rex::graphics::XE_GPU_REG_SCRATCH_UMSK]) {
        const uint32_t address = registers_[rex::graphics::XE_GPU_REG_SCRATCH_ADDR] + n * 4;
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(address), value);
        GuestSeesGpuProgress();
      }
    }
  }

  // The guest can see the GPU move on (a fence value written to memory, a scratch register mirrored, an interrupt):
  // from there it may rewrite vertex and index memory the GPU already drew from. Dead Space 2 refills one dynamic
  // vertex buffer for its translucent draws this way, several times a frame at the same address. The renderer keeps
  // copies of ranges it already uploaded (in the work slot, and per epoch in the geometry store) and forgets them
  // when this counter moves; without it every translucent batch after the first was drawn with the first one's
  // vertices: the huge triangles across the screen.
  static void GuestSeesGpuProgress() {
#if DANTE_DEKO3D
    rex::x360::g_sincronizaciones_anillo.fetch_add(1, std::memory_order_relaxed);
#endif
  }

  // --- GPU memory (the two low bits of the address are the byte order) --------

  uint32_t ReadMemory(uint32_t address) const {
    uint32_t value;
    std::memcpy(&value, memory_->TranslatePhysical(address & ~uint32_t(0x3)), sizeof(value));
    return xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
  }

  void WriteMemory(uint32_t address, uint32_t value) {
    GuestSeesGpuProgress();
    const uint32_t swapped = xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
    std::memcpy(memory_->TranslatePhysical(address & ~uint32_t(0x3)), &swapped, sizeof(swapped));
  }

  // --- Threads ----------------------------------------------------------------

  // Creates the deko renderer backend on first use (on the ring thread, as the device allows). The game plugs in no
  // effects, so it uses the default hooks.
  bool EnsureRenderer() {
    if (destinos_) {
      return true;
    }
    if (!provider_ || !presenter_) {
      return false;
    }
    destinos_ = rex::x360::CrearDestinosDeko3d(
        static_cast<rex::deko3d::ProveedorDeko3d*>(provider_.get()), memory_);
    if (destinos_) {
      REXLOG_INFO("[gpu] deko3d renderer backend up (minimal renderer: draws, resolves and the Swap's frontbuffer)");
    }
    return destinos_ != nullptr;
  }

  // Runs the game's interrupt callback on the calling thread (one of ours, an XHostThread).
  void Interrupt(uint32_t source, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) {
      return;
    }
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) {
      return;
    }
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
  }

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    while (active_.load(std::memory_order_acquire)) {
      const auto now = Clock::now();
      // After a long pause (loading, a debugger) the missed vblanks are not replayed.
      if (now - next > std::chrono::milliseconds(250)) {
        next = now;
      }
      while (now >= next) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        vblanks_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  }

  int RingLoop() {
    uint32_t read = 0;
    uint32_t generation = 0;
    while (active_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_cv_.wait_for(lock, std::chrono::milliseconds(4), [&] {
          return !active_.load(std::memory_order_acquire) ||
                 write_pointer_.load(std::memory_order_acquire) != read ||
                 ring_generation_.load(std::memory_order_acquire) != generation;
        });
      }
      if (!active_.load(std::memory_order_acquire)) {
        break;
      }
      Report();
      const uint32_t gen = ring_generation_.load(std::memory_order_acquire);
      if (gen != generation) {
        generation = gen;
        read = 0;  // InitializeRingBuffer leaves the read pointer at zero
      }
      const uint32_t base = ring_base_.load(std::memory_order_acquire);
      const uint32_t words = ring_words_.load(std::memory_order_acquire);
      if (!base || !words) {
        continue;
      }
      const uint32_t written = write_pointer_.load(std::memory_order_acquire) & (words - 1);
      if (written == read) {
        continue;
      }
      Reader reader;
      reader.base = memory_->TranslatePhysical(base);
      reader.mask = words - 1;
      reader.pos = read;
      reader.end = written;
      while (reader.Pending()) {
        if (!Packet(reader, 0)) {
          break;  // partial packet: the rest comes with the next CP_RB_WPTR
        }
      }
      read = reader.pos;
#if DANTE_DEKO3D
      if (destinos_) {
        destinos_->EsperarSubidas();
      }
#endif
      const uint32_t writeback = read_pointer_writeback_.load(std::memory_order_acquire);
      if (writeback) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), read);
        // Not a GuestSeesGpuProgress: it is written after every batch of the ring, and each one made the renderer's
        // geometry store check every range again (an epoch). Ring memory comes back only after a wrap, frames later.
      }
    }
    return 0;
  }

  // One line every kReportEvery, on the ring thread, so the log shows whether the game is advancing.
  void Report() {
    const auto now = Clock::now();
    if (now - last_report_ < kReportEvery) {
      return;
    }
    const double seconds = std::chrono::duration<double>(now - last_report_).count();
    last_report_ = now;
    REXLOG_INFO("[gpu] {:.1f} s: {} swaps ({:.1f}/s), {} draws, {} packets, {} vblanks, {} waits cut short",
                seconds, swaps_ - reported_swaps_, double(swaps_ - reported_swaps_) / seconds,
                draws_ - reported_draws_, packets_ - reported_packets_,
                vblanks_.load(std::memory_order_relaxed), waits_cut_short_);
    reported_swaps_ = swaps_;
    reported_draws_ = draws_;
    reported_packets_ = packets_;
    REXLOG_INFO("[gpu] ring waits: {} WAIT_REG_MEM waited {:.1f} ms in total", reg_mem_waits_,
                double(reg_mem_wait_ns_) / 1e6);
    reg_mem_waits_ = 0;
    reg_mem_wait_ns_ = 0;
#if DANTE_DEKO3D
    const dante::shaders::Estadisticas s = dante::shaders::Global().estadisticas();
    REXLOG_INFO("[gpu] shaders: {} loads (address {}, address refused {}, learned {}, microcode {}, similar {}, "
                "unknown {}); objects {} ({} not in the library, {} without a microcode address)",
                s.cargas, s.por_direccion, s.direccion_rechazada, s.por_aprendizaje, s.por_microcodigo,
                s.por_parecido, s.desconocidas,
                s.objetos, s.objetos_sin_entrada, s.objetos_sin_direccion);
    // What the draws sent to the renderer target and draw (ColorRenderTargetFormat of each written color
    // target, DepthRenderTargetFormat when depth is on, primitive type): the formats it does not handle yet show
    // up here with how many draws they cost.
    std::string formats;
    for (uint32_t f = 0; f < 16; ++f) {
      if (color_formats_[f]) formats += fmt::format(" c{}:{}", f, color_formats_[f]);
    }
    for (uint32_t f = 0; f < 2; ++f) {
      if (depth_formats_[f]) formats += fmt::format(" z{}:{}", f, depth_formats_[f]);
    }
    std::string primitives;
    for (uint32_t t = 0; t < 32; ++t) {
      if (primitive_types_[t]) primitives += fmt::format(" {}:{}", t, primitive_types_[t]);
    }
    std::string orders;
    static constexpr const char* kOrder[4] = {"none", "8in16", "8in32", "16in32"};
    for (uint32_t k = 0; k < 8; ++k) {
      if (index_orders_[k]) orders += fmt::format(" {}-bit {}:{}", k < 4 ? 16 : 32, kOrder[k & 3], index_orders_[k]);
    }
    index_orders_ = {};
#if DANTE_DEKO3D
    const uint32_t syncs = rex::x360::g_sincronizaciones_anillo.load(std::memory_order_relaxed);
    const uint32_t syncs_interval = syncs - syncs_reported_;
    syncs_reported_ = syncs;
#else
    const uint32_t syncs_interval = 0;
#endif
    REXLOG_INFO("[gpu] draw targets:{}; primitive types:{}; indices:{}; {} emulated clears, {} multisampled draws drawn "
                "at 1x; {} guest-visible GPU progress points (renderer copies forgotten)", formats, primitives, orders,
                clears_, msaa_draws_, syncs_interval);
    color_formats_ = {};
    depth_formats_ = {};
    primitive_types_ = {};
    uint64_t dibujados = 0, rechazados = 0;
    if (destinos_) {
      const rex::x360::EstadisticasDibujos e = destinos_->EstadisticasDeDibujos();
      dibujados = e.dibujados;
      rechazados = e.rechazados;
    }
    REXLOG_INFO("[gpu] draws: {} geometry, {} sent to the renderer: {} drawn, {} rejected by it; skipped here: "
                "{} without VS, {} without PS (the renderer logs each rejection cause once)",
                geometria_, enviados_, dibujados, rechazados, sin_vs_, sin_ps_);
#endif
  }

  // --- PM4 packets ------------------------------------------------------------

  // Consumes a whole packet, or leaves the position untouched if it is not complete yet.
  bool Packet(Reader& reader, int depth) {
    const uint32_t packet = reader.Peek();
    uint32_t count = 0;
    switch (packet >> 30) {
      case 0: count = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0; break;
      case 1: count = 2; break;
      case 2: count = 0; break;
      default: count = ((packet >> 16) & 0x3FFF) + 1; break;
    }
    if (reader.Pending() < count + 1) {
      return false;
    }
    reader.Skip(1);
    ++packets_;
    Reader data = reader;
    reader.Skip(count);
    if (!packet) {
      return true;
    }
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool one_register = (packet >> 15) & 0x1;
        for (uint32_t i = 0; i < count; ++i) {
          WriteRegister(one_register ? index : index + i, data.Read());
        }
        break;
      }
      case 1: {
        const uint32_t value1 = data.Read();
        const uint32_t value2 = data.Read();
        WriteRegister(packet & 0x7FF, value1);
        WriteRegister((packet >> 11) & 0x7FF, value2);
        break;
      }
      case 2:
        break;
      default:
        PacketType3(data, packet, count, depth);
        break;
    }
    return true;
  }

  void PacketType3(Reader& data, uint32_t packet, uint32_t count, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    if ((packet & 0x1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      return;  // predicate fails; predicated Swaps are always dropped
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        if (count < 1) break;
        const uint32_t cpus = data.Read();
        GuestSeesGpuProgress();
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) {
            Interrupt(1, cpu);
          }
        }
        break;
      }
      case xenos::PM4_XE_SWAP:
        // VdSwap writes "SWAP", the frontbuffer's physical address and its size. The renderer presents whatever is
        // resolved (clears and, once the ring decoder feeds draws, the frame). Handheld 1280x720.
        ++swaps_;
        counter_.fetch_add(1, std::memory_order_relaxed);
        if (EnsureRenderer()) {
          // VdSwap describes the frontbuffer in fetch constant 0; an empty TexturaSwap finds nothing (black).
          destinos_->Presentar(presenter_.get(), rex::x360::LeerTexturaSwap(registers_.data()), 1280, 720);
        }
        break;
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (count < 2) break;
        const uint32_t address = data.Read() & 0x1FFFFFFF;
        const uint32_t length = data.Read() & 0xFFFFF;
        if (depth < kMaxIndirectDepth) {
          Reader indirect;
          indirect.base = memory_->TranslatePhysical(address);
          indirect.end = length;
          while (indirect.Pending() && Packet(indirect, depth + 1)) {
          }
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        if (count < 5) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const auto start = Clock::now();
        const auto limit = start + kWaitRegMemMax;
        const auto yield_until = start + std::chrono::milliseconds(1);
        bool waited = false;
        for (;;) {
          if (!(info & 0x10) && poll == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST &&
              (Register(poll) & UINT32_C(0x80000000))) {
            registers_[poll] = 0;  // MakeCoherent: there is no GPU cache to flush
          }
          const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
          if (Compare(info, value & mask, reference)) {
            break;
          }
          if (Clock::now() >= limit || !active_.load(std::memory_order_acquire)) {
            if (waits_cut_short_++ == 0) {
              REXLOG_WARN("[gpu] WAIT_REG_MEM not met ({} {:08X} ref {:08X} mask {:08X}), going on",
                          (info & 0x10) ? "memory" : "register", poll, reference, mask);
            }
            break;
          }
          waited = true;
          // The first millisecond yielding instead of sleeping: a sleep on Horizon often lasts far longer than asked,
          // and the game's main thread waits on the GPU fences behind this wait (D3D BlockOnFence in its profile).
          if (Clock::now() < yield_until) {
            rex::thread::MaybeYield();
          } else {
            rex::thread::Sleep(std::chrono::microseconds(100));
          }
        }
        if (waited) {
          ++reg_mem_waits_;
          reg_mem_wait_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (count < 3) break;
        const uint32_t info = data.Read();
        const uint32_t and_value = data.Read();
        const uint32_t or_value = data.Read();
        uint32_t value = Register(info & 0x1FFF);
        value &= ((info >> 31) & 0x1) ? Register(and_value & 0x1FFF) : and_value;
        value |= ((info >> 30) & 0x1) ? Register(or_value & 0x1FFF) : or_value;
        WriteRegister(info & 0x1FFF, value);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        if (count < 2) break;
        const uint32_t reg = data.Read();
        const uint32_t address = data.Read();
        WriteMemory(address, Register(reg));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        if (count < 1) break;
        uint32_t address = data.Read();
        for (uint32_t i = 1; i < count; ++i) {
          WriteMemory(address, data.Read());
          address += 4;
        }
        break;
      }
      case xenos::PM4_COND_WRITE: {
        if (count < 6) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const uint32_t target = data.Read();
        const uint32_t value_to_write = data.Read();
        const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
        if (Compare(info, value & mask, reference)) {
          if (info & 0x100) {
            WriteMemory(target, value_to_write);
          } else {
            WriteRegister(target, value_to_write);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (count < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        if (count < 3) break;
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        const uint32_t value = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        WriteMemory(address, ((initiator >> 31) & 0x1) ? counter_.load(std::memory_order_relaxed) : value);
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        if (count < 2) break;
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        // Screen extent of the previous draw: the maximum, as the SDK does.
        const uint16_t extent[] = {0, kMaxExtent, 0, kMaxExtent, 0, 1};
        uint8_t* target = memory_->TranslatePhysical(address & ~uint32_t(0x3));
        for (size_t i = 0; i < std::size(extent); ++i) {
          rex::memory::store_and_swap<uint16_t>(target + i * 2, extent[i]);
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        if (count < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        OcclusionQuery(Register(rex::graphics::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR));
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (count < 1) break;
        const uint32_t type_index = data.Read();
        const uint32_t base = ConstantBase(type_index);
        if (base != UINT32_MAX) {
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 1; i < count; ++i) {
            WriteRegister(index + i - 1, data.Read());
          }
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        // First word: the register index; the rest is written consecutively.
        if (count < 1) break;
        const uint32_t index = data.Read() & 0xFFFF;
        for (uint32_t i = 1; i < count; ++i) {
          WriteRegister(index + i - 1, data.Read());
        }
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        // Registers loaded from guest memory instead of from the ring.
        if (count < 3) break;
        const uint32_t address = data.Read() & 0x3FFFFFFF;
        const uint32_t type_index = data.Read();
        const uint32_t n = data.Read() & 0xFFF;
        const uint32_t base = ConstantBase(type_index);
        if (base != UINT32_MAX) {
          const uint8_t* source = memory_->TranslatePhysical(address);
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 0; i < n; ++i) {
            WriteRegister(index + i, rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4));
          }
        }
        break;
      }
      case xenos::PM4_VIZ_QUERY: {
        // When a query ends it is reported as visible, in case the game reads the result (as the SDK does).
        if (count < 1) break;
        const uint32_t word = data.Read();
        const uint32_t id = word & 0x3F;
        if (!(word & 0x100)) {
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_START);
        } else {
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_END);
          const uint32_t reg = id < 32 ? uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0)
                                       : uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1);
          registers_[reg] |= UINT32_C(1) << (id < 32 ? id : id - 32);
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (count < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (count < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (count < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (count < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK: {
        if (count < 2) break;
        const uint64_t high = data.Read();
        bin_mask_ = (high << 32) | data.Read();
        break;
      }
      case xenos::PM4_SET_BIN_SELECT: {
        if (count < 2) break;
        const uint64_t high = data.Read();
        bin_select_ = (high << 32) | data.Read();
        break;
      }
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        ++draws_;
#if DANTE_DEKO3D
        // DRAW_INDX starts with the visibility query word. Then the initiator and, for indices in memory (DMA),
        // their base and size, kept in their registers as the SDK's command processor does: the renderer reads
        // everything from the register mirror.
        const uint32_t skip = opcode == xenos::PM4_DRAW_INDX ? 1 : 0;
        if (count < skip + 1) break;
        if (skip) data.Read();
        const uint32_t initiator = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR, initiator);
        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA)) {
          if (count < skip + 3) break;
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_BASE, data.Read());
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_SIZE, data.Read());
        }
        if (!EnsureRenderer()) break;
        // A copy-mode draw is a resolve and/or a clear.
        if (rex::x360::EsCopia(registers_.data())) {
          destinos_->Copiar(rex::x360::LeerRegistrosCopia(registers_.data()));
          break;
        }
        Draw();
#else
        (void)data;
#endif
        break;
      }
#if DANTE_DEKO3D
      case xenos::PM4_IM_LOAD: {
        // Microcode in GPU memory; the type (0 VS, 1 PS) in the two low bits of the address.
        if (count < 2) break;
        const uint32_t address_type = data.Read();
        const uint32_t words = data.Read() & 0xFFFF;
        const uint32_t type = address_type & 0x3;
        if (type > 1 || !words) break;
        const uint8_t* source = memory_->TranslatePhysical(address_type & ~uint32_t(0x3));
        microcode_.resize(words);
        for (uint32_t i = 0; i < words; ++i) {
          microcode_[i] = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
        }
        ShaderLoaded(type, address_type & ~uint32_t(0x3));
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {
        // Microcode inside the packet (D3D's copy path, patched there).
        if (count < 2) break;
        const uint32_t type = data.Read() & 0x3;
        const uint32_t words = data.Read() & 0xFFFF;
        if (type > 1 || !words || words > count - 2) break;
        microcode_.resize(words);
        for (uint32_t i = 0; i < words; ++i) {
          microcode_[i] = data.Read();
        }
        ShaderLoaded(type, 0);
        break;
      }
#endif
      default:
        // IM_LOAD, IM_LOAD_IMMEDIATE, INVALIDATE_STATE, NOP...: nothing the game waits for.
        break;
    }
  }

#if DANTE_DEKO3D
  // PA_SC_WINDOW_OFFSET..SCISSOR_BR, PA_CL_VPORT_*, PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL and
  // PA_SU_VTX_CNTL: the viewport/scissor state the renderer caches (as the SDK's command processor).
  static bool EsRegistroDeEncuadre(uint32_t index) {
    if (index - 0x2080 > 0x2302 - 0x2080) return false;
    return index <= 0x2082 || (index >= 0x210F && index <= 0x2114) || (index >= 0x2204 && index <= 0x2206) ||
           index == 0x2302;
  }

  // An IM_LOAD / IM_LOAD_IMMEDIATE of type 0 (VS) or 1 (PS), its microcode in microcode_ (host order).
  void ShaderLoaded(uint32_t type, uint32_t physical) {
    const rex::x360::EntradaShader* entry = dante::shaders::Global().Identificar(type == 0, physical, microcode_);
    if (type == 1) {
      ps_ = entry;
      return;
    }
    // The same vertex shader loaded again from the same address (D3D reloads it for every draw that sets it): its
    // patched microcode and generation are the ones already here. Checked by length and three words, as the library
    // does, instead of copying and hashing it again.
    const size_t n = microcode_.size();
    if (physical && entry && entry == vs_ && physical == vs_physical_ && n && n == vs_microcode_.size() &&
        microcode_[0] == vs_microcode_[0] && microcode_[n / 2] == vs_microcode_[n / 2] &&
        microcode_[n - 1] == vs_microcode_[n - 1]) {
      return;
    }
    vs_physical_ = physical;
    vs_ = entry;
    vs_microcode_ = microcode_;  // patched by D3D: the vertex input is read from here
    // The same microcode keeps its generation, so the renderer's per-generation caches stay valid.
    const uint64_t key = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t)) ^
                         vs_microcode_.size();
    auto [it, inserted] = vs_generations_.try_emplace(key, vs_generations_.size() + 1);
    generacion_vs_ = it->second;
  }

  void Draw() {
    ++geometria_;
    namespace g = rex::graphics;
    // Depth only (RB_MODECONTROL 5): the GPU runs no pixel shader, so the VS is enough.
    const bool depth_only = (Register(g::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5;
    const uint32_t msaa = (Register(g::XE_GPU_REG_RB_SURFACE_INFO) >> 16) & 0x3;
    // Which way the scene's depth test goes, for the depth value of the emulated clears.
    if (!msaa && (Register(g::XE_GPU_REG_RB_DEPTHCONTROL) & 0x2)) {
      const uint32_t zfunc = (Register(g::XE_GPU_REG_RB_DEPTHCONTROL) >> 4) & 0x7;
      if (zfunc == 4 || zfunc == 6) ++depth_greater_; else if (zfunc == 1 || zfunc == 3) ++depth_less_;
    }
    // Direct3D clears a target by drawing over it with its own internal shaders (not created by the game, so not in
    // the library), through the same EDRAM seen as a multisampled surface of half the pitch. Done as the renderer's
    // clears instead.
    // Only full-surface ones (window scissor to 8192): Dead Space 2's light stencil masks are multisampled draws too,
    // and one whose VS was not identified, taken for a clear, wiped the stencil and let a light flood the screen.
    // A single-sampled full-surface draw without an identified VS is usually a real draw (the lamps' 64x8 visibility
    // pass is one, and clearing for it wiped the scene's depth in the middle of the frame). Single-sampled, only
    // Direct3D's own clear shape is taken: one rectangle (rect list, 3 vertices) with all four targets' mask. The
    // decompression scene clears its quarter-size particle buffer that way every frame; left as it was, the smoke and
    // debris of every frame piled up into grey smears. And only onto an 8-bit color target with a color in 0..1 in the
    // clear shader's c0: the same shape also fills the 80-wide float exposure target with game data (c0 like 0.027 0
    // -18.4 1), which is not a clear.
    const uint32_t initiator = Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    bool rect_clear = !msaa && (initiator & 0x3F) == 8 && (initiator >> 16) == 3 &&
                      Register(g::XE_GPU_REG_RB_COLOR_MASK) == 0xFFFF &&
                      ((Register(g::XE_GPU_REG_RB_COLOR_INFO) >> 16) & 0xF) <= 1;
    for (uint32_t j = 0; j < 4 && rect_clear; ++j) {
      float v;
      std::memcpy(&v, &registers_[0x4400 + j], 4);
      rect_clear = v >= 0.0f && v <= 1.0f;
    }
    if (!vs_ && ((msaa && (Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR) & 0x3FFF) >= 0x2000) || rect_clear)) {
      EmulateClear(msaa);
      return;
    }
    if (!vs_) {
      ++sin_vs_;
      // A real draw whose VS was not identified (not a Direct3D clear): which microcode, once each.
      if (!msaa && unknown_vs_logged_.size() < 16) {
        const uint64_t key = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t));
        if (unknown_vs_logged_.insert(key).second) {
          REXLOG_WARN("[gpu] draw skipped, VS not identified: microcode {:016X} ({} words, first {:08X} {:08X} "
                      "{:08X}), target c0 {:08X} pitch {}, PS n{}",
                      key, vs_microcode_.size(), vs_microcode_.size() > 0 ? vs_microcode_[0] : 0,
                      vs_microcode_.size() > 1 ? vs_microcode_[1] : 0, vs_microcode_.size() > 2 ? vs_microcode_[2] : 0,
                      Register(g::XE_GPU_REG_RB_COLOR_INFO), Register(g::XE_GPU_REG_RB_SURFACE_INFO) & 0x3FFF,
                      ps_ ? int(ps_->numero) : -1);
        }
      }
      return;
    }
    if (!ps_ && !depth_only) {
      ++sin_ps_;
      return;
    }
    // Diagnostics (Report): formats and primitive types of what is sent.
    const uint32_t mask = Register(rex::graphics::XE_GPU_REG_RB_COLOR_MASK);
    static constexpr uint32_t kColorInfo[4] = {
        rex::graphics::XE_GPU_REG_RB_COLOR_INFO, rex::graphics::XE_GPU_REG_RB_COLOR1_INFO,
        rex::graphics::XE_GPU_REG_RB_COLOR2_INFO, rex::graphics::XE_GPU_REG_RB_COLOR3_INFO};
    for (uint32_t i = 0; i < 4 && ps_ && !depth_only; ++i) {
      if (((mask >> (i * 4)) & 0xF) && ((ps_->salidas >> i) & 0x1)) {
        ++color_formats_[(Register(kColorInfo[i]) >> 16) & 0xF];
      }
    }
    if (Register(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL) & 0x6) {  // Z test or Z write
      ++depth_formats_[(Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO) >> 16) & 0x1];
    }
    ++primitive_types_[Register(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR) & 0x1F];
    {
      // Indexed draws by index size and byte order (VGT_DMA_SIZE bits 30-31): 16-bit indices in k8in32 or k16in32
      // were read unswapped by the SDK.
      const uint32_t init = Register(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR);
      if (((init >> 6) & 0x3) == 0) {  // source select: DMA (an index buffer)
        ++index_orders_[((init >> 11) & 0x1) * 4 + (Register(rex::graphics::XE_GPU_REG_VGT_DMA_SIZE) >> 30)];
      }
    }

    rex::x360::PeticionDibujo request;
    request.registros = registers_.data();
    AliasScope alias(*this);
    if (msaa) {
      // Direct3D's trick for quarter-rate passes (Dead Space 2's light stencil masks): the 1280x720 depth buffer
      // drawn as a 640x360 4x-multisampled surface over the same EDRAM, each pixel's four samples landing on 2x2
      // pixels. The renderer has no multisampling and keys targets by pitch, so the draw goes to the 1x surface
      // with everything in pixels scaled up instead (same pixels covered).
      request.registros = alias.Apply(msaa);
      ++generacion_encuadre_;  // the scaled viewport and scissor are not the mirror's
    }
    request.vs = vs_;
    request.ps = ps_;
    request.vs_microcodigo = vs_microcode_;
    request.generacion_vs = generacion_vs_;
    request.generacion_constantes_vs = generacion_constantes_vs_;
    request.generacion_constantes_ps = generacion_constantes_ps_;
    request.generacion_fetch = generacion_fetch_;
    request.generacion_encuadre = generacion_encuadre_;
    ++enviados_;
    destinos_->Dibujar(request);
    if (msaa) {
      ++generacion_encuadre_;  // back to the mirror's for the next draw
      ++msaa_draws_;
    }
  }

  // The register mirror with a multisampled surface turned into the 1x surface it aliases: 4X doubles width and
  // height (pitch x2), 2X doubles the height (samples stacked vertically). Viewport and scissors scale the same way.
  // Done in place on the ten registers involved and undone when the scope ends (copying the whole 80 KB mirror for
  // each of the ~30 multisampled draws per frame cost ~2.4 MB of copies per frame on the ring thread).
  class AliasScope {
   public:
    explicit AliasScope(RingGraphicsSystem& system) : system_(system) {}
    ~AliasScope() {
      for (uint32_t i = 0; i < count_; ++i) system_.registers_[saved_[i].first] = saved_[i].second;
    }
    const uint32_t* Apply(uint32_t msaa) { return system_.AliasedRegisters(msaa, *this); }
    void Save(uint32_t index) { saved_[count_++] = {index, system_.registers_[index]}; }

   private:
    RingGraphicsSystem& system_;
    std::pair<uint32_t, uint32_t> saved_[12];
    uint32_t count_ = 0;
  };

  const uint32_t* AliasedRegisters(uint32_t msaa, AliasScope& scope) {
    namespace g = rex::graphics;
    for (uint32_t index : {uint32_t(g::XE_GPU_REG_RB_SURFACE_INFO), uint32_t(g::XE_GPU_REG_PA_CL_VPORT_XSCALE),
                           uint32_t(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 1), uint32_t(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 2),
                           uint32_t(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 3), uint32_t(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL),
                           uint32_t(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR), uint32_t(g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL),
                           uint32_t(g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR)}) {
      scope.Save(index);
    }
    uint32_t* r = registers_.data();
    const uint32_t sx = msaa == 2 ? 2 : 1, sy = 2;
    const uint32_t surface = r[g::XE_GPU_REG_RB_SURFACE_INFO];
    r[g::XE_GPU_REG_RB_SURFACE_INFO] = (surface & ~uint32_t(0x33FFF)) | std::min<uint32_t>((surface & 0x3FFF) * sx, 0x3FFF);
    auto scale_float = [&](uint32_t index, uint32_t factor) {
      float v;
      std::memcpy(&v, &r[index], 4);
      v *= float(factor);
      std::memcpy(&r[index], &v, 4);
    };
    scale_float(g::XE_GPU_REG_PA_CL_VPORT_XSCALE, sx);
    scale_float(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 1, sx);  // XOFFSET
    scale_float(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 2, sy);  // YSCALE
    scale_float(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 3, sy);  // YOFFSET
    auto scale_xy = [&](uint32_t index, uint32_t bits) {
      const uint32_t v = r[index];
      const uint32_t mask = (1u << bits) - 1;
      const uint32_t x = std::min((v & mask) * sx, mask), y = std::min(((v >> 16) & mask) * sy, mask);
      r[index] = (v & ~(mask | (mask << 16))) | x | (y << 16);
    };
    scale_xy(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL, 14);
    scale_xy(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, 14);
    scale_xy(g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL, 15);
    scale_xy(g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR, 15);
    return r;
  }

  // A Direct3D clear draw, as the renderer's clears. Direct3D's fast clear binds c0 (the color to clear) and, for the
  // depth buffer, c1-c3 over the depth buffer's EDRAM written as color, with a placeholder depth target that may even
  // sit on c0's base (Dead Space 2's light buffer clear: c0 2D0, c1-c3 000, depth 2D0). So: c0 is cleared as color
  // (to 0); c1-c3 on another base than c0 clear the depth buffer at their base (far plane, stencil 0); the bound
  // depth target is cleared only when depth or stencil is written and it is not on c0's base; depth-only mode
  // clears no color. The first few are logged with what the game had set, to check.
  void EmulateClear(uint32_t msaa) {
    namespace g = rex::graphics;
    AliasScope alias(*this);
    const uint32_t* r = msaa ? alias.Apply(msaa) : registers_.data();
    rex::x360::RegistrosCopia base = rex::x360::LeerRegistrosCopia(r);
    // The whole surface: the renderer takes a copy's rectangle from three vertices in fetch constant 0 (where
    // Direct3D puts a resolve's), clipped by the window scissor. Ours, in a small guest buffer made once.
    if (!clear_rect_) {
      clear_rect_ = memory_->SystemHeapAlloc(32, 32, rex::memory::kSystemHeapPhysical);
    }
    if (!clear_rect_) return;
    const float w = float(r[g::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF), h = 8192.0f;
    const float rect[6] = {0.0f, 0.0f, w, 0.0f, 0.0f, h};  // host order, read with Endian::kNone
    std::memcpy(memory_->TranslateVirtual<uint8_t*>(clear_rect_), rect, sizeof(rect));
    const uint32_t physical = memory_->GetPhysicalAddress(clear_rect_);
    base.fetch_vertices[0] = (physical & ~uint32_t(0x3)) | uint32_t(xenos::FetchConstantType::kVertex);
    base.fetch_vertices[1] = (6u << 2) | uint32_t(xenos::Endian::kNone);
    base.pa_sc_window_offset = 0;
    base.pa_sc_window_scissor_tl = 0x80000000u;  // window_offset_disable, from 0,0
    base.pa_sc_window_scissor_br = 0x3FFF3FFFu;
    base.pa_su_sc_mode_cntl &= ~(1u << 16);       // no vtx_window_offset
    const uint32_t mask = r[g::XE_GPU_REG_RB_COLOR_MASK];
    const uint32_t depth_info = r[g::XE_GPU_REG_RB_DEPTH_INFO];
    const uint32_t depth_control = r[g::XE_GPU_REG_RB_DEPTHCONTROL];
    const bool depth_only = (r[g::XE_GPU_REG_RB_MODECONTROL] & 0x7) == 5;
    const bool reversed = depth_greater_ > depth_less_;
    // The stencil: Direct3D's clear quad writes the stencil reference with a REPLACE (depthcontrol 00008777: stencil on,
    // ALWAYS, z-pass REPLACE), not 0. Dead Space 2 clears it to its own value before the menu's Scaleform masks, whose
    // frames test LESS against it; written as 0, every frame and backing panel of the menus failed and only the text
    // (drawn without stencil) was left.
    uint32_t stencil = 0;
    if ((depth_control & 0x1) && ((depth_control >> 14) & 0x7) == 2) {
      const uint32_t ref_mask = r[g::XE_GPU_REG_RB_STENCILREFMASK];
      stencil = ref_mask & (ref_mask >> 16) & 0xFF;
    }
    const uint32_t depth_value = (reversed ? 0u : 0xFFFFFF00u) | stencil;
    constexpr uint32_t kNullCommand = 3u << 20;  // CopyCommand::kNull: clear only, copy nothing
    auto clear_depth_at = [&](uint32_t edram_base, std::string& what) {
      rex::x360::RegistrosCopia c = base;
      c.rb_depth_info = (depth_info & ~uint32_t(0xFFF)) | (edram_base & 0xFFF);
      c.rb_copy_control = 4u | (1u << 9) | kNullCommand;
      c.rb_depth_clear = depth_value;
      destinos_->Copiar(c);
      what += fmt::format(" z={:03X}->{:08X}", edram_base & 0xFFF, depth_value);
    };
    std::string what;
    const uint32_t c0_base = base.rb_color_info[0] & 0xFFF;
    const bool c0_written = !depth_only && (mask & 0xF);
    // The color: the multisampled ones were all seen clearing to 0. A single-sampled one takes what Direct3D's clear
    // shader writes, the pixel shader's c0 (RGBA8 for the renderer's clear).
    uint32_t color = 0;
    if (!msaa) {
      for (uint32_t j = 0; j < 4; ++j) {
        float v;
        std::memcpy(&v, &registers_[0x4400 + j], 4);
        color |= uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)) << (j * 8);
      }
    }
    if (c0_written) {
      rex::x360::RegistrosCopia c = base;
      c.rb_copy_control = 0u | (1u << 8) | kNullCommand;
      c.rb_color_clear = color;
      c.rb_color_clear_lo = 0;
      destinos_->Copiar(c);
      what += fmt::format(" c0={:03X}", c0_base);
      if (color) what += fmt::format("={:08X}", color);
    }
    uint32_t aliased_depth[3] = {};
    uint32_t n_aliased = 0;
    for (uint32_t i = 1; i < 4 && !depth_only; ++i) {
      const uint32_t b = base.rb_color_info[i] & 0xFFF;
      if (!((mask >> (i * 4)) & 0xF) || (c0_written && b == c0_base)) continue;
      if (std::find(aliased_depth, aliased_depth + n_aliased, b) != aliased_depth + n_aliased) continue;
      aliased_depth[n_aliased++] = b;
      clear_depth_at(b, what);
    }
    const uint32_t z_base = depth_info & 0xFFF;
    const bool z_written = depth_only || (depth_control & 0x4) || (depth_control & 0x1);
    if (z_written && !(c0_written && z_base == c0_base) &&
        std::find(aliased_depth, aliased_depth + n_aliased, z_base) == aliased_depth + n_aliased) {
      clear_depth_at(z_base, what);
      // Depth written over c0's EDRAM (Dead Space 2's light buffer clear: depth-only mode, depth target on 2D0 where
      // the light buffer is): on the console the same EDRAM bytes are the color target's, so the color is cleared by
      // it (depth 0 and stencil 0 there: black). The renderer keeps depth and color in separate images, so the color
      // image is cleared too; without it the lights were added on top of the normals left there by the G-buffer.
      if (z_base == c0_base && (mask & 0xF)) {
        rex::x360::RegistrosCopia c = base;
        c.rb_copy_control = 0u | (1u << 8) | kNullCommand;
        c.rb_color_clear = 0;
        c.rb_color_clear_lo = 0;
        destinos_->Copiar(c);
        what += fmt::format(" c0={:03X} (by the depth over it)", c0_base);
      }
    }
    ++clears_;
    if (clear_shapes_.insert(what).second && clear_shapes_.size() <= 32) {
      REXLOG_INFO("[gpu] clear shape: msaa {}, mode {}:{}", msaa, r[g::XE_GPU_REG_RB_MODECONTROL] & 0x7, what);
    }
    // Single-sampled ones counted apart: they are new, and their game clear color (PS c0) tells whether 0 is right.
    if (msaa ? clears_logged_ < 12 : clears_logged_1x_ < 8) {
      ++(msaa ? clears_logged_ : clears_logged_1x_);
      auto f = [&](uint32_t index) {
        float v;
        std::memcpy(&v, &registers_[index], 4);
        return v;
      };
      REXLOG_INFO("[gpu] clear draw (msaa {}, pitch {}):{}; game had: PS c0 ({:.3f} {:.3f} {:.3f} {:.3f}) VS c0 "
                  "({:.3f} {:.3f} {:.3f} {:.3f}) RB_COLOR_CLEAR {:08X} RB_DEPTH_CLEAR {:08X} depthcontrol {:08X} "
                  "mask {:04X}",
                  msaa, Register(g::XE_GPU_REG_RB_SURFACE_INFO) & 0x3FFF, what, f(0x4400), f(0x4401), f(0x4402),
                  f(0x4403), f(0x4000), f(0x4001), f(0x4002), f(0x4003), Register(g::XE_GPU_REG_RB_COLOR_CLEAR),
                  Register(g::XE_GPU_REG_RB_DEPTH_CLEAR), depth_control, mask);
    }
  }

#endif

  // EVENT_WRITE_ZPD, as the SDK's CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD: D3D marks the
  // structure with 0xFFFFFEED on Issue(END), and the counts are faked.
  void OcclusionQuery(uint32_t address) {
    if (!address) {
      return;
    }
    const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEEDu);  // written big-endian, the struct is LE
    auto* counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
    const bool end_via_z_pass = counts->ZPass_A == kQueryFinished || counts->ZPass_B == kQueryFinished;
    const bool end_via_z_fail = counts->ZFail_A == kQueryFinished || counts->ZFail_B == kQueryFinished;
    std::memset(counts, 0, sizeof(*counts));
    if (end_via_z_pass || end_via_z_fail) {
      counts->ZPass_A = kFakeSampleCount;
      counts->Total_A = kFakeSampleCount;
    }
  }

  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::GraphicsProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  std::unique_ptr<rex::x360::DestinosNativos> destinos_;  // the universal deko renderer backend
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  bool mmio_registered_ = false;
  std::atomic<bool> active_{false};

  std::vector<uint32_t> registers_;
  uint64_t bin_mask_ = ~uint64_t(0);
  uint64_t bin_select_ = ~uint64_t(0);

  std::atomic<uint32_t> callback_{0};
  std::atomic<uint32_t> callback_data_{0};
  std::atomic<uint32_t> ring_base_{0};
  std::atomic<uint32_t> ring_words_{0};
  std::atomic<uint32_t> ring_generation_{0};
  std::atomic<uint32_t> write_pointer_{0};
  std::atomic<uint32_t> read_pointer_writeback_{0};
  std::atomic<uint32_t> counter_{0};  // vblanks + swaps, for EVENT_WRITE_SHD
  std::atomic<uint64_t> vblanks_{0};
  std::mutex ring_mutex_;
  std::condition_variable ring_cv_;
  rex::system::object_ref<rex::system::XHostThread> ring_thread_;
  rex::system::object_ref<rex::system::XHostThread> vblank_thread_;

  // Only the ring thread touches these.
#if DANTE_DEKO3D
  std::vector<uint32_t> microcode_;     // the last IM_LOAD, host byte order
  std::vector<uint32_t> vs_microcode_;  // the last VS load, as D3D patched it
  const rex::x360::EntradaShader* vs_ = nullptr;
  const rex::x360::EntradaShader* ps_ = nullptr;
  std::unordered_map<uint64_t, uint64_t> vs_generations_;
  uint64_t generacion_vs_ = 0, generacion_constantes_vs_ = 0, generacion_constantes_ps_ = 0;
  uint64_t generacion_fetch_ = 0, generacion_encuadre_ = 0;
  uint64_t geometria_ = 0, enviados_ = 0, sin_vs_ = 0, sin_ps_ = 0;
  std::array<uint64_t, 16> color_formats_{};
  std::array<uint64_t, 2> depth_formats_{};
  std::array<uint64_t, 32> primitive_types_{};
  std::unordered_set<std::string> clear_shapes_;  // EmulateClear
  uint32_t clear_rect_ = 0;          // guest buffer with the emulated clears' rectangle
  uint64_t depth_greater_ = 0, depth_less_ = 0, clears_ = 0, clears_logged_ = 0, msaa_draws_ = 0;
  std::array<uint64_t, 8> index_orders_{};
  uint32_t vs_physical_ = 0;
  uint32_t syncs_reported_ = 0;
  uint64_t clears_logged_1x_ = 0;
  std::unordered_set<uint64_t> unknown_vs_logged_;
#endif
  uint64_t swaps_ = 0, draws_ = 0, packets_ = 0, waits_cut_short_ = 0;
  uint64_t reg_mem_waits_ = 0, reg_mem_wait_ns_ = 0;  // WAIT_REG_MEM that had to wait, and how long in total
  uint64_t reported_swaps_ = 0, reported_draws_ = 0, reported_packets_ = 0;
  Clock::time_point last_report_;
};

}  // namespace

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem() {
  return std::make_unique<RingGraphicsSystem>();
}

}  // namespace dante::gpu
