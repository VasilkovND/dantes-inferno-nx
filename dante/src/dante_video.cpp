// dantes-inferno-nx - the FMVs' VP6 frames decoded by FFmpeg instead of the game's own decoder.
//
// The FMVs are EA VP6, 1280x720 at 29.97 fps. The game decodes them on its Job Manager thread with its own decoder,
// which recompiled (float IDCT on VMX, ~3,000 instructions per block in VP6_DecodeBlock) runs at ~18 frames a second
// on the console: the videos stutter, the thread pinned at 95 %. FFmpeg's VP6 decoder (sdk-patches/0009) does a frame of
// these in about a millisecond on a Mac core, so a few on the Switch's.
//
// Where it goes in: VP6_DecodeFrameMbs (0x826974D0), the decoder's macroblock pass. Its callers parse the frame header,
// store the frame's data at pbi+268, reset the row counter pbi+19300 for a new frame, and call it for a number of
// macroblock rows (the decode job, several times a frame) or all of them (-1); when the counter reaches the height in
// rows, the job publishes the frame buffer at pbi+492 (pbi+500, read by VP6_GetYUVConfig for display). Here, at the
// first row of a frame, FFmpeg decodes the whole frame into that buffer and the counter is set to the last row: the
// header parse and the buffer rotation stay the game's.
//
// The buffer (VP6_GetYUVConfig, 0x82693ED8): Y at frame + pbi[452] + 48 * (stride + 1) with stride pbi[388] (a 48-pixel
// border), U and V at frame + pbi[456 / 460] + 24 * uv_stride + 24 with uv_stride pbi[392]; width and height pbi[380]
// and pbi[384].
//
// The first frames of the first video are decoded by both: the game's output is compared with FFmpeg's, as it is and
// upside down (VP6 codes frames bottom up, and which way this buffer holds them is the game's business), until a frame
// tells the two apart (a uniform one, like the logos' black first frames, does not). The orientation it tells is used
// for that video and every later one (FFmpeg's integer IDCT and the game's float one differ by rounding only). A video
// whose pictures keep disagreeing stays with the game's decoder, as before. dante_vp6_ffmpeg=false turns it all off.

#include "dantes_inferno_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

extern "C" {
#include "libavcodec/avcodec.h"
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_BOOL(dante_vp6_ffmpeg, true, "Dante",
                    "Los videos (VP6) descodificados con FFmpeg en vez del descodificador del juego, mucho mas lento "
                    "recompilado. false = el del juego");

namespace {

// Fields of the game's decoder instance (pbi).
constexpr uint32_t kDatos = 268, kAncho = 380, kAlto = 384, kPasoY = 388, kPasoUV = 392, kDespY = 452, kDespU = 456,
                   kDespV = 460, kNuevo = 492, kFila = 19300;
// Mean absolute difference of Y (0-255) under which the game's frame and FFmpeg's are the same picture, and how much
// worse the other orientation must be for a frame to tell which one it is: a uniform frame (the black first frames of
// the logo videos) looks the same both ways and tells nothing.
constexpr double kParecido = 3.0;
constexpr double kSeparacion = 4.0;
// Frames the game's picture and FFmpeg's may disagree before a video stays with the game's decoder.
constexpr uint32_t kDesacuerdos = 5;

enum class Modo { kSinCalibrar, kFFmpeg, kFFmpegVolteado, kJuego };

struct Decodificador {
  AVCodecContext* c = nullptr;
  AVFrame* f = nullptr;
  AVPacket* p = nullptr;
  Modo modo = Modo::kSinCalibrar;
  std::vector<uint8_t> paquete;
  uint64_t cuadros = 0, errores = 0;
  uint32_t calibrando = 0, desacuerdos = 0;
  double ms = 0;

  Decodificador() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_VP6);
    if (!codec) return;
    c = avcodec_alloc_context3(codec);
    c->thread_count = 1;
    if (avcodec_open2(c, codec, nullptr) < 0) {
      avcodec_free_context(&c);
      return;
    }
    f = av_frame_alloc();
    p = av_packet_alloc();
  }
  ~Decodificador() {
    av_packet_free(&p);
    av_frame_free(&f);
    avcodec_free_context(&c);
  }
  bool listo() const { return c && f && p; }

  bool Decodificar(const uint8_t* datos, uint32_t bytes) {
    paquete.assign(datos, datos + bytes);
    paquete.resize(size_t(bytes) + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    p->data = paquete.data();
    p->size = int(bytes);
    if (avcodec_send_packet(c, p) < 0) return false;
    return avcodec_receive_frame(c, f) == 0;
  }
};

std::mutex g_candado;
std::unordered_map<uint32_t, std::unique_ptr<Decodificador>> g_decodificadores;  // by pbi
// The orientation, once one video has told it: the same buffer code for every video, so later ones start with FFmpeg.
Modo g_orientacion = Modo::kSinCalibrar;

// The frame's bytes: the EA chunk ("MV0K" key frame, "MV0F" other, little-endian size with its 8-byte header) when the
// data sits right after its header; otherwise as far as its memory region goes, at most 1 MB (VP6 reads only what the
// frame has, the rest is never looked at).
uint32_t BytesDelCuadro(uint8_t* base, uint32_t datos) {
  const uint8_t* cabecera = REX_RAW_ADDR(datos - 8);
  if (!std::memcmp(cabecera, "MV0K", 4) || !std::memcmp(cabecera, "MV0F", 4)) {
    uint32_t n;
    std::memcpy(&n, cabecera + 4, 4);
    if (n > 8 && n < (4u << 20)) return n - 8;
  }
  rex::memory::Memory* memoria = rex::system::kernel_memory();
  rex::memory::BaseHeap* heap = memoria ? memoria->LookupHeap(datos) : nullptr;
  rex::memory::HeapAllocationInfo info{};
  if (heap && heap->QueryRegionInfo(datos, &info) && (info.state & rex::memory::kMemoryAllocationCommit)) {
    const uint64_t fin = uint64_t(info.base_address) + info.region_size;
    if (fin > datos) return uint32_t(std::min<uint64_t>(fin - datos, 1u << 20));
  }
  return 64u << 10;
}

struct Planos {
  uint32_t y, u, v, paso_y, paso_uv, ancho, alto;
};

Planos PlanosDe(uint8_t* base, uint32_t pbi) {
  const uint32_t cuadro = REX_LOAD_U32(pbi + kNuevo);
  Planos pl{};
  pl.paso_y = REX_LOAD_U32(pbi + kPasoY);
  pl.paso_uv = REX_LOAD_U32(pbi + kPasoUV);
  pl.ancho = REX_LOAD_U32(pbi + kAncho);
  pl.alto = REX_LOAD_U32(pbi + kAlto);
  pl.y = cuadro + REX_LOAD_U32(pbi + kDespY) + 48 * (pl.paso_y + 1);
  pl.u = cuadro + REX_LOAD_U32(pbi + kDespU) + 24 * pl.paso_uv + 24;
  pl.v = cuadro + REX_LOAD_U32(pbi + kDespV) + 24 * pl.paso_uv + 24;
  return pl;
}

bool Encaja(const Planos& pl, const AVFrame* f) {
  return pl.ancho && pl.alto && int(pl.ancho) <= f->width && int(pl.alto) <= f->height && pl.paso_y >= pl.ancho &&
         pl.paso_uv >= pl.ancho / 2;
}

void Escribir(uint8_t* base, const Planos& pl, const AVFrame* f, bool volteado) {
  auto plano = [&](uint32_t destino, uint32_t paso, int i, uint32_t ancho, uint32_t alto) {
    for (uint32_t fila = 0; fila < alto; ++fila) {
      const uint32_t origen = volteado ? alto - 1 - fila : fila;
      std::memcpy(REX_RAW_ADDR(destino + fila * paso), f->data[i] + size_t(origen) * f->linesize[i], ancho);
    }
  };
  plano(pl.y, pl.paso_y, 0, pl.ancho, pl.alto);
  plano(pl.u, pl.paso_uv, 1, pl.ancho / 2, pl.alto / 2);
  plano(pl.v, pl.paso_uv, 2, pl.ancho / 2, pl.alto / 2);
}

// Mean absolute difference of Y between the game's frame (as its decoder left it) and FFmpeg's, on a grid.
double Diferencia(uint8_t* base, const Planos& pl, const AVFrame* f, bool volteado) {
  uint64_t suma = 0, n = 0;
  for (uint32_t fila = 0; fila < pl.alto; fila += 4) {
    const uint8_t* juego = REX_RAW_ADDR(pl.y + fila * pl.paso_y);
    const uint8_t* ffmpeg = f->data[0] + size_t(volteado ? pl.alto - 1 - fila : fila) * f->linesize[0];
    for (uint32_t x = 0; x < pl.ancho; x += 4) {
      suma += uint32_t(std::abs(int(juego[x]) - int(ffmpeg[x])));
      ++n;
    }
  }
  return n ? double(suma) / double(n) : 1e9;
}

double AhoraMs() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// void VP6_DecodeFrameMbs(pbi, int rows): rows from pbi[19300] (all of them with -1).
REX_EXTERN(__imp__VP6_DecodeFrameMbs);
REX_EXTERN(VP6_DecodeFrameMbs) {
  const uint32_t pbi = ctx.r3.u32;
  if (!REXCVAR_GET(dante_vp6_ffmpeg) || REX_LOAD_U32(pbi + kFila) != 0) {
    __imp__VP6_DecodeFrameMbs(ctx, base);
    return;
  }
  std::lock_guard<std::mutex> cerrojo(g_candado);
  std::unique_ptr<Decodificador>& d = g_decodificadores[pbi];
  const uint32_t datos = REX_LOAD_U32(pbi + kDatos);
  const uint8_t* cuadro = REX_RAW_ADDR(datos);
  const bool clave = !(cuadro[0] & 0x80);  // VP6: frame type bit, 0 = key frame
  if (!d) {
    d = std::make_unique<Decodificador>();
    if (!d->listo()) {
      REXLOG_WARN("[video] FFmpeg sin descodificador VP6: los videos siguen con el del juego");
      d->modo = Modo::kJuego;
    }
  }
  if (d->modo == Modo::kJuego) {
    __imp__VP6_DecodeFrameMbs(ctx, base);
    return;
  }
  const uint32_t bytes = BytesDelCuadro(base, datos);
  const double t0 = AhoraMs();
  const bool ok = d->Decodificar(cuadro, bytes);
  const double ms = AhoraMs() - t0;
  const Planos pl = PlanosDe(base, pbi);

  if (d->modo == Modo::kSinCalibrar && g_orientacion != Modo::kSinCalibrar && ok && Encaja(pl, d->f)) {
    d->modo = g_orientacion;
    REXLOG_INFO("[video] VP6 {:08X} {}x{}: FFmpeg{}, como el primer video", pbi, pl.ancho, pl.alto,
                g_orientacion == Modo::kFFmpegVolteado ? ", volteado" : "");
  }
  if (d->modo == Modo::kSinCalibrar) {
    // The game's decoder does this frame, all rows; then both pictures are compared.
    ctx.r4.u64 = ~uint64_t(0);
    __imp__VP6_DecodeFrameMbs(ctx, base);
    ++d->calibrando;
    if (!ok || !Encaja(pl, d->f)) {
      REXLOG_WARN("[video] VP6 {:08X}: FFmpeg {} ({}x{} contra {}x{}, pasos {} {}): sigue el descodificador del juego", pbi,
                  ok ? "no encaja" : "no descodifica el cuadro", d->f->width, d->f->height, pl.ancho, pl.alto,
                  pl.paso_y, pl.paso_uv);
      d->modo = Modo::kJuego;
      return;
    }
    const double recto = Diferencia(base, pl, d->f, false), volteado = Diferencia(base, pl, d->f, true);
    const bool usar_volteado = volteado < recto;
    const double mejor = std::min(recto, volteado), peor = std::max(recto, volteado);
    if (mejor < kParecido && peor - mejor >= kSeparacion) {
      d->modo = usar_volteado ? Modo::kFFmpegVolteado : Modo::kFFmpeg;
      g_orientacion = d->modo;
    } else if (mejor >= kParecido && ++d->desacuerdos >= kDesacuerdos) {
      d->modo = Modo::kJuego;
    } else {
      return;  // a uniform frame (or one disagreement): the next frame is compared too
    }
    REXLOG_INFO("[video] VP6 {:08X} {}x{} (cuadro {}, {} bytes{}): diferencia media con el del juego {:.2f} recto, {:.2f} "
                "volteado -> {}",
                pbi, pl.ancho, pl.alto, d->calibrando, bytes, clave ? ", clave" : "", recto, volteado,
                d->modo == Modo::kJuego ? "descodificador del juego" : (usar_volteado ? "FFmpeg, volteado" : "FFmpeg"));
    return;
  }

  if (!ok) {
    // FFmpeg refused the frame: the game's decoder takes this video from here (its reference frames are stale until
    // the next key frame).
    REXLOG_WARN("[video] VP6 {:08X}: FFmpeg no descodifica un cuadro ({} bytes): sigue el descodificador del juego", pbi,
                bytes);
    d->modo = Modo::kJuego;
    __imp__VP6_DecodeFrameMbs(ctx, base);
    return;
  }
  Escribir(base, pl, d->f, d->modo == Modo::kFFmpegVolteado);
  REX_STORE_U32(pbi + kFila, pl.alto / 16);  // every macroblock row done
  d->ms += ms;
  if (++d->cuadros % 300 == 0) {
    REXLOG_INFO("[video] VP6 {:08X}: {} cuadros con FFmpeg, {:.1f} ms de media por cuadro", pbi, d->cuadros,
                d->ms / 300.0);
    d->ms = 0;
  }
}
