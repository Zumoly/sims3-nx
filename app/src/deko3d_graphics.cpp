#include "deko3d_graphics.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <set>
#include <sys/stat.h>

#include <fmt/format.h>
#include <rex/filesystem.h>

#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>

#include <cstddef>

#include <deko3d.hpp>
#include <switch.h>

#include <pthread.h>

#include <rex/system/util/object_table.h>

#include "sims3_ui_shaders.h"

extern "C" char _start;
extern "C" uint32_t RexSwitchHandleFor(pthread_t thread);  // sdk/src/core/threading_switch.h

namespace sims3 {
namespace {

using namespace rex;  // X_STATUS and its values
namespace xenos = rex::graphics::xenos;
using Clock = std::chrono::steady_clock;

// Frame pacing: no frame is shown sooner than 1/cap s after the last one (a late frame is not held back further).
// fps.txt next to the NRO holds the cap (0 for none), by default 30. The port does not change clock rates: for
// speed, users run an overclock tool (sys-clk, hoc-clk) with a profile for the game.
namespace clocks {
uint32_t cap_fps = 30;
u64 last_present = 0;

void Start() {
  if (FILE* f = std::fopen((rex::filesystem::GetExecutableFolder() / "fps.txt").string().c_str(), "r")) {
    int c = 30;
    if (std::fscanf(f, "%d", &c) == 1 && c >= 0 && c <= 60) cap_fps = uint32_t(c);
    std::fclose(f);
  }
  REXLOG_INFO("[clock] fps cap {}", cap_fps);
}

void Stop() {}

// Before each present.
void Pace() {
  const u64 now = armGetSystemTick();
  if (cap_fps && last_present) {
    // 1.5 ms early: the flip itself waits for the next vertical blank.
    const u64 frame = armNsToTicks(1000000000ull / cap_fps - 1500000ull);
    if (now - last_present < frame) svcSleepThread(armTicksToNs(frame - (now - last_present)));
  }
  last_present = armGetSystemTick();
}
}  // namespace clocks

constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x0000FFFF;
constexpr uint32_t kRegisterCount = 0x5003;

constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegScratchUmsk = 0x01DC;
constexpr uint32_t kRegScratchAddr = 0x01DD;
constexpr uint32_t kRegScratch0 = 0x0578;
constexpr uint32_t kRegScratch7 = 0x057F;
constexpr uint32_t kRegCoherStatusHost = 0x0A31;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;
constexpr uint32_t kRegRbSampleCountAddr = 0x2325;
constexpr uint32_t kRegRbSurfaceInfo = 0x2000;
constexpr uint32_t kRegRbColorInfo = 0x2001;
constexpr uint32_t kRegRbDepthInfo = 0x2002;
constexpr uint32_t kRegRbColorMask = 0x2104;
constexpr uint32_t kRegRbStencilRefMask = 0x210D;
constexpr uint32_t kRegRbDepthControl = 0x2200;
constexpr uint32_t kRegRbModeControl = 0x2208;
constexpr uint32_t kRegFetchFirst = 0x4800;
constexpr uint32_t kRegFetchLast = 0x48BF;
constexpr uint32_t kEdramModeCopy = 6;
constexpr auto kCensusSummary = std::chrono::seconds(5);
constexpr uint32_t kCensusDetailEvery = 6;  // summaries between detailed reports

uint32_t ConstantBase(uint32_t type) {
  switch (type) {
    case 0: return 0x4000;  // ALU
    case 1: return 0x4800;  // fetch
    case 2: return 0x4900;  // bool
    case 3: return 0x4908;  // loop
    case 4: return 0x2000;  // registers
    default: return UINT32_MAX;
  }
}

// What the game asks of the GPU. Observation only: nothing here changes what the guest sees.
struct Census {
  // Reset at each summary.
  uint64_t swaps = 0, draws = 0, resolves = 0, predicated = 0, indices = 0, host_resolves = 0, depth_restores = 0;
  uint64_t colour_restores = 0;
  uint64_t ring_words = 0, indirect_buffers = 0, interrupts = 0;
  uint64_t waits = 0, wait_timeouts = 0, wait_ns = 0, busy_ns = 0, present_ns = 0;
  // Kept for the whole run.
  uint64_t total_swaps = 0, total_draws = 0, total_resolves = 0, shader_loads = 0;
  uint32_t largest_shader_dwords = 0, largest_texture_w = 0, largest_texture_h = 0;
  std::array<uint64_t, 64> primitive{};
  std::array<uint64_t, 4> source{};
  std::array<uint64_t, 64> texture_format{};
  std::array<uint64_t, 128> opcode{};
  std::map<uint32_t, uint64_t> targets;  // pitch | msaa << 14 | colour << 16 | depth << 20 | MRTs << 21
  struct WaitSite {
    uint64_t count = 0, timeouts = 0, ns = 0;
  };
  std::map<std::pair<uint32_t, uint32_t>, WaitSite> wait_sites;  // (address or register, ref)
  std::unordered_set<uint64_t> vertex_shaders, pixel_shaders, textures;
  uint32_t frontbuffer = 0, front_width = 0, front_height = 0;
  Clock::time_point last = Clock::now();
  uint32_t reports = 0;
};

// Saved for offline shader translation and renderer work. Stops after the last threshold.
// Draws per frame vary a lot by screen (13 to 130 seen on the menu), so each capture asks for more.
constexpr uint32_t kCaptureFrames = 2;
constexpr bool kRealCubeMaps = true;
constexpr bool kSurfaceDumps = false;  // every console fault so far came after a dump: kept off to compare
constexpr uint64_t kCaptureGapSwaps = 300;
constexpr uint32_t kCaptureMinDraws = 120;  // loading screens have far fewer
constexpr uint32_t kCaptureMinResolves = 10;
constexpr uint32_t kCaptureHeavyFrames = 90;  // three seconds at 30 fps
constexpr uint32_t kCaptureStableFrames = 8;  // a change after this many identical frames is a flash
constexpr size_t kDrawLogLimit = size_t(8) << 20;
constexpr uint64_t kCaptureMemoryLimit = uint64_t(240) << 20;  // per frame
constexpr uint64_t kCaptureGeometryLimit = uint64_t(120) << 20;  // vertices and indices, so textures still fit
constexpr uint32_t kCaptureLargestRange = 16u << 20;
constexpr uint32_t kTagRegisters = 0x53474552;  // 'REGS'
constexpr uint32_t kTagDraw = 0x57415244;       // 'DRAW'
constexpr uint32_t kTagMemory = 0x524D454D;     // 'MEMR'
constexpr uint32_t kTagSwap = 0x50415753;       // 'SWAP'

struct Capture {
  bool active = false;
  uint32_t frames_done = 0;
  uint64_t next_swap = UINT64_MAX;  // frames are off unless a test marker asks for them
  uint32_t frame_draws = 0;  // draws in the frame being built, captured or not
  uint32_t frame_resolves = 0;
  uint32_t heavy_frames = 0;
  uint32_t captured_draws = 0, skipped_ranges = 0;
  uint64_t geometry_bytes = 0;
  uint64_t memory_bytes = 0;
  uint64_t vertex_shader = 0, pixel_shader = 0;
  std::string root;
  std::vector<uint8_t> out;
  std::vector<uint32_t> previous;
  std::set<std::pair<uint32_t, uint32_t>> saved_ranges;
  std::unordered_set<uint64_t> saved_shaders;

  void Put32(uint32_t value) {
    const size_t at = out.size();
    out.resize(at + 4);
    std::memcpy(&out[at], &value, 4);
  }
  void Put64(uint64_t value) {
    Put32(uint32_t(value));
    Put32(uint32_t(value >> 32));
  }
};

uint64_t HashWords(const uint8_t* data, uint32_t dwords) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (uint32_t i = 0; i < dwords * 4; ++i) hash = (hash ^ data[i]) * 0x100000001B3ull;
  return hash;
}

constexpr int kMaxIndirectDepth = 4;
// The real GPU waits forever. A short limit let the ring run ahead of the CPU core it was waiting for.
constexpr auto kWaitMax = std::chrono::seconds(5);
constexpr uint32_t kFramebuffers = 2;
constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;

// Guest memory addresses in the ring carry a byte-order mode in their low two bits.
uint32_t GpuSwap(uint32_t value, uint32_t mode) {
  switch (mode & 3) {
    case 1: return ((value & 0xFF00FF00u) >> 8) | ((value & 0x00FF00FFu) << 8);
    case 2: return std::byteswap(value);
    case 3: return (value >> 16) | (value << 16);
    default: return value;
  }
}

bool Compare(uint32_t info, uint32_t value, uint32_t ref) {
  switch (info & 7) {
    case 0: return false;
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;
  }
}

constexpr uint32_t kVertexMemory = 16u << 20;
constexpr uint32_t kUniformMemory = DK_UNIFORM_BUF_ALIGNMENT;
constexpr uint32_t kDescriptorSlots = 4096;  // 32 bytes each, from data_memory_ + kVertexMemory + 0x1000
constexpr bool kDecodeSmallBc = true;  // small BC textures kept as RGBA8 (half icons)
// Moire (sims3_099: brick paving in the park rippled in rings): textures had only their full-size level, so distant
// surfaces skipped texels. Tiled power-of-two textures with a mip chain now get their guest levels, sampled trilinear.
constexpr bool kMipmaps = true;
constexpr float kAnisotropy = 4.0f;
constexpr uint32_t kTexturePool = 192u << 20;  // the world needs about 70 MB; at 96 MB the cache was wiped every few seconds
constexpr uint32_t kStagingMemory = 32u << 20;
// A/B switch (sims3_052: outdoor shade went black once stencil IDs survived the EDRAM reload).
constexpr bool kRestoreStencil = true;
// A/B switches for the speed caches (black outdoor shade, sims3_052-054): all true is the fast build.
constexpr size_t kFullHashBytes = 16u << 10;  // textures up to this size are always hashed whole (and Sim composites)
constexpr size_t kRestMinBytes = 64u << 10;    // textures up to this size are checked every frame
constexpr bool kTextureRest = true, kSampledHash = true, kKeepVertices = true, kConstantMasks = true;
constexpr uint32_t kCommandMemory = 8u << 20;  // a house frame runs to over 2000 draws
constexpr uint32_t kCommandExtra = 32u << 20;  // handed out on demand, for wide views of the world
constexpr uint32_t kShaderMemory = 0xC000;
constexpr uint32_t kXenosCodeMemory = 2u << 20;
constexpr uint32_t kChunkDraws = 160;  // draws per mid-frame submission
constexpr uint32_t kFrameMemory = 64u << 20;  // per frame slot (sims3_078: 7.4 MB in a busy house)
// Frames in flight: the game records frame N+1 while the GPU draws frame N. Each slot has its own command,
// frame, staging and interface-vertex memory and a fence; Begin waits only for the frame that last used the slot.
constexpr uint32_t kFrameSlots = 2;
constexpr uint32_t kVertexCache = 64u << 20;  // vertex buffers kept across frames  // constants, vertices and indices of one frame

// Every EDRAM target gets one host image of this size. The largest seen is pitch 1280 and 1024 rows.
constexpr uint32_t kTargetWidth = 1280;
constexpr uint32_t kTargetHeight = 1024;

struct UiVertex {
  float x, y, u, v;
  float c0[4];
  float c1[4];
};

// Must match the Draw block in ui.frag.glsl.
struct UiUniforms {
  float c2[4], c3[4], solid[4];
  int32_t mode[4];
};

struct TextureRequest {
  std::array<uint32_t, 4> key;
  const uint8_t* guest;
  uint32_t guest_bytes, format, width, height, pitch, endian;
  bool tiled;
  uint32_t swizzle[4];
  uint32_t signs = 0;  // fetch constant: two bits per component, 1 signed, 3 gamma
  bool cube = false;
  uint32_t slice_bytes = 0;  // distance between cube faces in guest memory
  bool full_hash = false;
  // Packed mips: a texture of 16 texels or less on its short side lives inside a 32x32 tile, at this offset.
  uint32_t packed_x = 0, packed_y = 0;
  // Mip levels 1 and on, from the fetch constant's mip address (kMipmaps).
  uint32_t mip_levels = 1;
  bool packed_mips = false;
  const uint8_t* mip_guest = nullptr;
  uint32_t mip_bytes = 0;
};

// One mip level of a tiled 2D texture in guest memory, relative to the mip address.
struct GuestMip {
  uint32_t offset = 0, bytes = 0, width = 1, height = 1, pitch = 32, packed_x = 0, packed_y = 0;
};

// Xenos mip layout (as Xenia reads it): level n is the power-of-two size >> n, stored with its pitch and height
// rounded up to 32 blocks, each level 4 KB aligned, level 1 at the mip address. With packed mips, the first level
// whose short side is 16 texels or less holds it and every smaller level in one tile, each at an offset.
// Fills out[1..levels-1]; returns the chain's size in bytes.
uint32_t GuestMips(uint32_t format, uint32_t width, uint32_t height, uint32_t levels, bool packed, GuestMip* out) {
  const bool compressed = format >= 18 && format <= 20;
  const uint32_t block = compressed ? 4 : 1;
  const uint32_t bytes = format == 18 ? 8 : compressed ? 16 : format == 2 ? 1 : (format == 3 || format == 10) ? 2 : 4;
  const uint32_t log2_w = std::countr_zero(width), log2_h = std::countr_zero(height);
  const uint32_t log2_min = std::min(log2_w, log2_h);
  const uint32_t packed_level = packed ? (log2_min > 4 ? log2_min - 4 : 0) : 99;
  uint32_t offset = 0;
  for (uint32_t level = 1; level < levels; ++level) {
    GuestMip& m = out[level];
    m.width = std::max(width >> level, 1u);
    m.height = std::max(height >> level, 1u);
    const uint32_t store = std::min(level, packed_level);
    const uint32_t sw = std::max(width >> store, 1u), sh = std::max(height >> store, 1u);
    const uint32_t pitch_blocks = (((sw + block - 1) / block) + 31) & ~31u;
    const uint32_t rows = (((sh + block - 1) / block) + 31) & ~31u;
    m.pitch = pitch_blocks * block;
    m.bytes = ((pitch_blocks * rows * bytes) + 0xFFF) & ~0xFFFu;
    if (level <= packed_level) {
      m.offset = offset;
      offset += m.bytes;
    } else {
      m.offset = out[packed_level].offset;
    }
    m.packed_x = m.packed_y = 0;
    if (level >= packed_level) {
      const uint32_t tail = level - packed_level;
      if (tail < 3) {
        (log2_w > log2_h ? m.packed_y : m.packed_x) = 16u >> tail;
      } else if (log2_w > log2_h) {
        m.packed_x = (1u << (log2_w - packed_level)) >> (tail - 2);
      } else {
        m.packed_y = (1u << (log2_h - packed_level)) >> (tail - 2);
      }
    }
  }
  return offset;
}

uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2) {
  pitch = (pitch + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2 + 7);
  const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2;
  const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         ((((y & 8) >> 2) + (x >> 3)) & 3) * 64 + (offset & 0x3F);
}

// Sampling suits 3D textures, where a new image replaces most of the data. Interface textures must not be
// sampled: the font and icon atlases change in small patches, and missed glyphs show up as squares.
uint64_t HashMemory(const uint8_t* data, size_t bytes, bool sampled) {
  uint64_t hash = 0xCBF29CE484222325ull ^ bytes;
  // 520, not 512: a linear texture's rows are often a multiple of 512 bytes, and samples that all land in one
  // column miss every change elsewhere (a lot's lightmap kept its first, empty copy and the floor went black).
  const size_t step = (sampled && bytes > 0x4000) ? 520 : 8;
  size_t i = 0;
  if (step == 8) {  // whole buffers: four independent lanes, so the loop runs at memory speed
    uint64_t lane[4] = {hash, hash ^ 1, hash ^ 2, hash ^ 3};
    for (; i + 32 <= bytes; i += 32) {
      uint64_t words[4];
      std::memcpy(words, data + i, 32);
      for (int k = 0; k < 4; ++k) lane[k] = (lane[k] ^ words[k]) * 0x9E3779B97F4A7C15ull;
    }
    for (int k = 0; k < 4; ++k) hash = (hash ^ lane[k] ^ (lane[k] >> 29)) * 0x9E3779B97F4A7C15ull;
  }
  for (; i + 8 <= bytes; i += step) {
    uint64_t word;
    std::memcpy(&word, data + i, 8);
    hash = (hash ^ word) * 0x9E3779B97F4A7C15ull;
    hash ^= hash >> 29;
  }
  return hash;
}

// Render-thread profile in system ticks, reported with the census every 5 s. Hash and upload are part of texture;
// submit is part of record.
enum ProfileSection {
  kProfVertex, kProfIndex, kProfTexture, kProfHash, kProfUpload, kProfConstants, kProfRecord, kProfSubmit,
  kProfResolve, kProfFlush, kProfClear, kProfDraw, kProfShaderHash, kProfPacket, kProfFence, kProfCount
};
uint64_t g_profile[kProfCount];
uint64_t g_vertex_bytes = 0, g_hash_bytes = 0, g_upload_bytes = 0, g_uploads = 0;
uint64_t g_vcache[3] = {};
// Speed (sims3_086: ~340 texture uploads and ~1 GB hashed per 5 s in town): which textures, by guest address.
struct TextureStat {
  uint32_t uploads = 0, checks = 0, format = 0, width = 0, height = 0;
  uint64_t hashed = 0, uploaded = 0;
  bool tiled = false, full = false;
};
std::unordered_map<uint32_t, TextureStat> g_texture_stats;  // cached vertex buffers used as they were, hashed and found unchanged, rewritten
struct ProfileScope {
  explicit ProfileScope(ProfileSection section) : section(section), start(armGetSystemTick()) {}
  ~ProfileScope() { g_profile[section] += armGetSystemTick() - start; }
  ProfileSection section;
  uint64_t start;
};

void LogDekoError(void*, const char* context, DkResult result, const char* message) {
  static uint32_t count = 0;
  if (++count > 300) return;
  REXLOG_ERROR("[deko3d] {}: result {}, {}", context ? context : "?", int(result), message ? message : "");
}

DkBlendFactor BlendFactor(uint32_t factor) {
  switch (factor) {
    case 0: return DkBlendFactor_Zero;
    case 1: return DkBlendFactor_One;
    case 4: return DkBlendFactor_SrcColor;
    case 5: return DkBlendFactor_InvSrcColor;
    case 6: return DkBlendFactor_SrcAlpha;
    case 7: return DkBlendFactor_InvSrcAlpha;
    case 8: return DkBlendFactor_DstColor;
    case 9: return DkBlendFactor_InvDstColor;
    case 10: return DkBlendFactor_DstAlpha;
    case 11: return DkBlendFactor_InvDstAlpha;
    case 12: return DkBlendFactor_ConstColor;
    case 13: return DkBlendFactor_InvConstColor;
    case 14: return DkBlendFactor_ConstAlpha;
    case 15: return DkBlendFactor_InvConstAlpha;
    case 16: return DkBlendFactor_SrcAlphaSaturate;
    default: return DkBlendFactor_One;
  }
}

DkImageSwizzle ImageSwizzle(uint32_t select) {
  switch (select) {
    case 0: return DkImageSwizzle_Red;
    case 1: return DkImageSwizzle_Green;
    case 2: return DkImageSwizzle_Blue;
    case 3: return DkImageSwizzle_Alpha;
    case 4: return DkImageSwizzle_Zero;
    default: return DkImageSwizzle_One;
  }
}

DkCompareOp CompareOp(uint32_t function) {
  static constexpr DkCompareOp kCompare[8] = {DkCompareOp_Never,  DkCompareOp_Less,    DkCompareOp_Equal,
                                              DkCompareOp_Lequal, DkCompareOp_Greater, DkCompareOp_NotEqual,
                                              DkCompareOp_Gequal, DkCompareOp_Always};
  return kCompare[function & 7];
}

bool GammaFetch(uint32_t signs) { return (signs & 0x3F) == 0x3F; }
bool SignedFetch(uint32_t signs) { return signs == 0x55; }

class Deko3dPresenter {
 public:
  struct Texture {
    uint32_t id = 0;
    float u_scale = 1, v_scale = 1;
    bool valid = false;
  };

  struct Arena {
    uint8_t* cpu = nullptr;
    DkGpuAddr gpu = 0;
  };

  struct LoadedShader {
    const XenosShader* info = nullptr;
    dk::Shader shader;
    bool loaded = false;
  };

  struct GenericDraw {
    dk::Shader* vertex = nullptr;
    dk::Shader* pixel = nullptr;
    DkGpuAddr vertex_constants = 0, pixel_constants = 0, state = 0;
    uint32_t fetch_count = 0;
    DkBufExtents fetch[8] = {};
    uint32_t vertex_texture_count = 0, pixel_texture_count = 0;
    uint32_t vertex_slot[16] = {}, pixel_slot[32] = {};
    DkResHandle vertex_handle[16] = {}, pixel_handle[32] = {};
    uint32_t blend = 0, write_mask = 0, depth_control = 0, stencil_word = 0, stencil_back = 0, alpha_control = 0;
    float alpha_ref = 0;
    float point_size = 1.0f;  // point lists only
    DkScissor scissor = {};
    DkPrimitive primitive = DkPrimitive_Triangles;
    uint32_t count = 0;
    bool indexed = false, index32 = false;
    DkGpuAddr indices = 0;
    int32_t vertex_offset = 0;
    uint64_t vertex_hash = 0, pixel_hash = 0;
    bool bias_on = false;
    float bias_constant = 0, bias_slope = 0;
    uint32_t cull = 0;  // PA_SU_SC_MODE_CNTL bits 0-2: cull front, cull back, clockwise front
  };

  bool Create() {
    device_ = dk::DeviceMaker{}.setCbDebug(LogDekoError).create();
    dk::ImageLayout layout;
    dk::ImageLayoutMaker{device_}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(kWidth, kHeight)
        .initialize(layout);
    const uint64_t align = layout.getAlignment();
    const uint64_t size = (layout.getSize() + align - 1) & ~(align - 1);
    const uint32_t block =
        uint32_t((size * kFramebuffers + DK_MEMBLOCK_ALIGNMENT - 1) & ~uint64_t(DK_MEMBLOCK_ALIGNMENT - 1));
    image_memory_ = dk::MemBlockMaker{device_, block}
                        .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                        .create();
    DkImage const* pointers[kFramebuffers];
    for (uint32_t i = 0; i < kFramebuffers; ++i) {
      images_[i].initialize(layout, image_memory_, uint32_t(size * i));
      pointers[i] = &images_[i];
    }
    swapchain_ = dk::SwapchainMaker{device_, nwindowGetDefault(), pointers, kFramebuffers}.create();
    clocks::Start();
    queue_ = dk::QueueMaker{device_}.setFlags(DkQueueFlags_Graphics).create();

    const auto uncached = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    command_memory_ =
        dk::MemBlockMaker{device_, (kCommandMemory * 2 + kCommandExtra) * kFrameSlots}.setFlags(uncached).create();
    commands_ = dk::CmdBufMaker{device_}.setUserData(this).setCbAddMem(AddCommandMemory).create();
    commands_.addMemory(command_memory_, 0, kCommandMemory);
    uploads_ = dk::CmdBufMaker{device_}.create();
    uploads_.addMemory(command_memory_, kCommandMemory, kCommandMemory);

    data_memory_ = dk::MemBlockMaker{device_, kVertexMemory + 0x1000 + ((kDescriptorSlots * 32 + 0xFFF) & ~0xFFFu)}.setFlags(uncached).create();
    ui_vertex_memory_ = dk::MemBlockMaker{device_, kVertexMemory * kFrameSlots}.setFlags(uncached).create();
    vertices_ = static_cast<UiVertex*>(ui_vertex_memory_.getCpuAddr());
    uniform_address_ = data_memory_.getGpuAddr() + kVertexMemory;
    image_descriptors_ = reinterpret_cast<dk::ImageDescriptor*>(
        static_cast<uint8_t*>(data_memory_.getCpuAddr()) + kVertexMemory + 0x1000);
    sampler_descriptor_ = reinterpret_cast<dk::SamplerDescriptor*>(
        static_cast<uint8_t*>(data_memory_.getCpuAddr()) + kVertexMemory + 0x400);
    dk::Sampler sampler;
    sampler.setFilter(DkFilter_Linear, DkFilter_Linear, DkMipFilter_Linear);
    sampler.setMaxAnisotropy(kAnisotropy);
    sampler.setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge);
    sampler_descriptor_->initialize(sampler);

    texture_memory_ = dk::MemBlockMaker{device_, kTexturePool}
                          .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                          .create();
    staging_memory_ = dk::MemBlockMaker{device_, kStagingMemory * kFrameSlots}.setFlags(uncached).create();
    readback_memory_ = dk::MemBlockMaker{device_, kReadbackSlot * kFrameSlots}.setFlags(uncached).create();

    shader_memory_ = dk::MemBlockMaker{device_, kShaderMemory}
                         .setFlags(uncached | DkMemBlockFlags_Code)
                         .create();
    auto* code = static_cast<uint8_t*>(shader_memory_.getCpuAddr());
    uint32_t offset = 0;
    const auto load = [&](const uint8_t* data, uint32_t size, dk::Shader& shader) {
      std::memcpy(code + offset, data, size);
      dk::ShaderMaker{shader_memory_, offset}.initialize(shader);
      offset = (offset + size + 0xFF) & ~0xFFu;
    };
    load(k_ui_vert, sizeof(k_ui_vert), vertex_shader_);
    load(k_ui_frag, sizeof(k_ui_frag), fragment_shader_);
    load(k_blit_vert, sizeof(k_blit_vert), blit_vertex_);
    load(k_blit_frag, sizeof(k_blit_frag), blit_fragment_);
    load(k_blit_depth_frag, sizeof(k_blit_depth_frag), blit_depth_fragment_);
    load(k_blit_stencil_frag, sizeof(k_blit_stencil_frag), blit_stencil_fragment_);
    load(k_stencil_bit_frag, sizeof(k_stencil_bit_frag), stencil_bit_fragment_);

    xenos_memory_ = dk::MemBlockMaker{device_, kXenosCodeMemory}.setFlags(uncached | DkMemBlockFlags_Code).create();
    // Cached: about 20 MB is written here each frame, and uncached writes dominated the frame time.
    frame_memory_ = dk::MemBlockMaker{device_, kFrameMemory * kFrameSlots}
                        .setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached)
                        .create();
    vertex_cache_memory_ = dk::MemBlockMaker{device_, kVertexCache}
                               .setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached)
                               .create();
    for (const XenosShader& info : k_xenos_shaders) shaders_[info.hash].info = &info;
    // All of them now: nothing goes into code memory while a scene is loading.
    uint32_t preloaded = 0;
    for (auto& [hash, entry] : shaders_) {
      const uint32_t size = entry.info->size;
      if (xenos_used_ + size + DK_SHADER_CODE_UNUSABLE_SIZE > kXenosCodeMemory) break;
      std::memcpy(static_cast<uint8_t*>(xenos_memory_.getCpuAddr()) + xenos_used_, entry.info->code, size);
      dk::ShaderMaker{xenos_memory_, xenos_used_}.initialize(entry.shader);
      xenos_used_ = (xenos_used_ + size + 0xFF) & ~0xFFu;
      entry.loaded = true;
      ++preloaded;
    }
    REXLOG_INFO("[render] {} of {} shaders loaded at startup, {} KB of {} KB", preloaded, shaders_.size(),
                xenos_used_ >> 10, kXenosCodeMemory >> 10);

    dk::Sampler repeat;
    repeat.setFilter(DkFilter_Linear, DkFilter_Linear, DkMipFilter_Linear);
    repeat.setMaxAnisotropy(kAnisotropy);
    repeat.setWrapMode(DkWrapMode_Repeat, DkWrapMode_Repeat, DkWrapMode_Repeat);
    sampler_descriptor_[1].initialize(repeat);

    dk::ImageLayout cube_layout;
    dk::ImageLayoutMaker{device_}
        .setType(DkImageType_Cubemap)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(1, 1, 6)
        .initialize(cube_layout);
    const uint32_t cube_size =
        uint32_t((cube_layout.getSize() + DK_MEMBLOCK_ALIGNMENT - 1) & ~uint64_t(DK_MEMBLOCK_ALIGNMENT - 1));
    cube_memory_ = dk::MemBlockMaker{device_, cube_size}
                       .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image | DkMemBlockFlags_ZeroFillInit)
                       .create();
    cube_image_.initialize(cube_layout, cube_memory_, 0);
    dk::ImageView cube_view{cube_image_};
    cube_descriptor_ = --persistent_descriptor_;
    image_descriptors_[cube_descriptor_].initialize(cube_view);
    // Every other slot starts as a harmless image. The memory is otherwise whatever it held before, and the
    // GPU reads descriptors ahead in groups, so a slot not yet assigned must still name something mapped.
    MakeImage(DkImageFormat_RGBA8_Unorm, 1, 1, safe_memory_, safe_image_);
    dk::ImageView safe_view{safe_image_};
    for (uint32_t id = 0; id < cube_descriptor_; ++id) image_descriptors_[id].initialize(safe_view);
    return true;
  }

  LoadedShader* FindShader(uint64_t hash) {
    const auto it = shaders_.find(hash);
    if (it == shaders_.end()) return nullptr;
    LoadedShader& entry = it->second;
    if (!entry.loaded) {
      const uint32_t size = entry.info->size;
      if (xenos_used_ + size + DK_SHADER_CODE_UNUSABLE_SIZE > kXenosCodeMemory) return nullptr;
      std::memcpy(static_cast<uint8_t*>(xenos_memory_.getCpuAddr()) + xenos_used_, entry.info->code, size);
      dk::ShaderMaker{xenos_memory_, xenos_used_}.initialize(entry.shader);
      xenos_used_ = (xenos_used_ + size + 0xFF) & ~0xFFu;
      entry.loaded = true;
      Remember(fmt::format("shader {:016x}, {} bytes", hash, size));
      uploads_.barrier(DkBarrier_None, DkInvalidateFlags_Shader);
      uploaded_ = true;
    }
    return &entry;
  }

  // Bump allocator over one frame; the frame is finished before the next one starts.
  // Vertex buffers kept across frames, converted once (kVertexCache). Cleared whole at a frame start when full;
  // the generation tells users their entries are gone.
  Arena AllocateVertexCache(size_t bytes) {
    const size_t start = (vertex_cache_used_ + 15) & ~size_t(15);
    if (start + bytes > kVertexCache) {
      vertex_cache_full_ = true;
      return {};
    }
    vertex_cache_used_ = start + bytes;
    return {static_cast<uint8_t*>(vertex_cache_memory_.getCpuAddr()) + start, vertex_cache_memory_.getGpuAddr() + start};
  }
  void FlushVertexCache(const uint8_t* cpu, size_t bytes) {
    const size_t offset = cpu - static_cast<const uint8_t*>(vertex_cache_memory_.getCpuAddr());
    vertex_cache_memory_.flushCpuCache(uint32_t(offset), uint32_t(bytes));
  }
  uint32_t vertex_cache_generation() const { return vertex_cache_generation_; }
  size_t vertex_cache_used() const { return vertex_cache_used_; }
  uint32_t write_epoch() const { return write_epoch_; }
  uint64_t frame() const { return frame_; }

  Arena Allocate(size_t bytes, size_t align) {
    const size_t start = (frame_used_ + align - 1) & ~(align - 1);
    if (start + bytes > frame_base_ + kFrameMemory) return {};
    frame_used_ = start + bytes;
    return {static_cast<uint8_t*>(frame_memory_.getCpuAddr()) + start, frame_memory_.getGpuAddr() + start};
  }

  // Binds the host images standing for an EDRAM colour and depth surface.
  void SetTarget(uint32_t colour_key, uint32_t depth_key) {
    Target* colour = FindTarget(colours_, colour_key, false);
    Target* depth = FindTarget(depths_, depth_key, true);
    if (colour == current_colour_ && depth == current_depth_) return;
    current_colour_ = colour;
    current_depth_ = depth;
    dk::ImageView colour_view{colour->image};
    dk::ImageView depth_view{depth->image};
    commands_.bindRenderTargets(&colour_view, &depth_view);
    commands_.setViewports(0, {DkViewport{0.0f, 0.0f, float(kTargetWidth), float(kTargetHeight), 0.0f, 1.0f}});
  }

  // The whole target, or only `area` (clears honour the scissor).
  void Clear(uint32_t colour_mask, const float* colour, bool clear_depth, float depth, uint32_t stencil_mask,
             uint32_t stencil, const DkScissor* area = nullptr) {
    commands_.setScissors(0, {area ? *area : DkScissor{0, 0, kTargetWidth, kTargetHeight}});
    commands_.bindColorWriteState(dk::ColorWriteState{}.setMask(0, DkColorMask_RGBA));
    if (colour_mask) {
      commands_.clearColor(0, colour_mask & DkColorMask_RGBA, colour[0], colour[1], colour[2], colour[3]);
    }
    if (clear_depth || stencil_mask) {
      commands_.bindDepthStencilState(dk::DepthStencilState{}.setDepthWriteEnable(clear_depth));
      commands_.clearDepthStencil(clear_depth, depth, uint8_t(stencil_mask), uint8_t(stencil));
    }
  }

  // Copies part of an EDRAM target into a guest surface. dest_address may lie inside a surface: Direct3D
  // resolves the second tile of a frame to an address further into the same texture.
  void Resolve(uint32_t key, bool depth, bool swap, uint32_t dest_address, uint32_t dest_width, uint32_t dest_height,
               uint32_t source_x, uint32_t source_y, uint32_t dest_x, uint32_t dest_y, uint32_t width,
               uint32_t height, bool plain_rgba) {
    Target* source = FindTarget(depth ? depths_ : colours_, key, depth);
    Surface* surface = nullptr;
    Surface* shorter = nullptr;
    for (const auto& known : surfaces_) {
      if (known->base != dest_address || known->depth != depth || known->width != dest_width) continue;
      (known->height >= dest_height ? surface : shorter) = known.get();
    }
    // Direct3D builds an atlas by resolving each piece to its own address inside the texture, on tile
    // boundaries, and the first tile's address plus a tile offset is how a frame's second half arrives too.
    if (!surface && !shorter) {
      for (const auto& known : surfaces_) {
        uint32_t tile_x, tile_y;
        if (known->depth != depth || known->width != dest_width || dest_address <= known->base ||
            dest_address - known->base >= known->bytes ||
            !TileOrigin(dest_address - known->base, known->width, known->height, &tile_x, &tile_y)) {
          continue;
        }
        surface = known.get();
        dest_x = tile_x + (dest_x & 31);
        dest_y = tile_y + (dest_y & 31);
        if (known->width == 1024) ++atlas_events_;
        break;
      }
    }
    if (shorter) {  // an atlas filled row by row keeps the rows it has
      const uint32_t old_height = shorter->height;
      const uint32_t old_view = ViewOf(shorter, kIdentitySwizzle, 0);
      Retire(shorter);
      surface = CreateSurface(dest_address, dest_width, dest_height, depth);
      dk::ImageView grown_view{surface->image};
      Blit(old_view, grown_view, surface->width, surface->height, 0, 0, 0, 0, surface->width, old_height, depth ? 1 : 0);
      surface->filled_width = surface->width;
      surface->filled_height = old_height;
    }
    if (!surface) {
      // A smaller surface at this address is replaced; a larger one is an atlas with a temporary at its
      // start, and they live side by side, told apart by the size a fetch asks for.
      for (bool again = true; again;) {
        again = false;
        for (const auto& known : surfaces_) {
          if (known->base == dest_address &&
              (known->depth != depth || known->width < dest_width || known->height < dest_height)) {
            Retire(known.get());
            again = true;
            break;
          }
        }
      }
      surface = CreateSurface(dest_address, dest_width, dest_height, depth);
      if (dest_width == 1024 && dest_height <= 512) ++atlas_events_;
      // Pieces written back are in memory, so a new atlas starts from there. Memory nothing was written to holds
      // only leftovers, so it is used only when such a piece exists; surface pieces are copied over it either way.
      if (plain_rgba && dest_width <= 1024 && HasWrittenPiece(surface)) InitFromGuest(surface);
      MergePieces(surface);
    }
    surface->swapped = swap;
    if (dest_x >= surface->width || dest_y >= surface->height) return;
    width = std::min(width, surface->width - dest_x);
    height = std::min(height, surface->height - dest_y);
    surface->filled_width = std::max(surface->filled_width, dest_x + width);
    surface->filled_height = std::max(surface->filled_height, dest_y + height);
    dk::ImageView view{surface->image};
    Blit(source->descriptor, view, surface->width, surface->height, source_x, source_y, dest_x, dest_y, width, height,
         depth ? 1 : swap ? 2 : 0);
    if (depth) {
      if (!surface->has_stencil) {
        MakeImage(DkImageFormat_R8_Unorm, surface->width, surface->height, surface->stencil_memory,
                  surface->stencil_image);
        dk::ImageView stencil_view{surface->stencil_image};
        surface->views[kStencilView] = NewDescriptor(stencil_view);
        surface->has_stencil = true;
      }
      if (highlight_frame_ == frame_ && !stencil_root_.empty() && stencil_dumps_ == 0) {
        SaveImage(source->image, kTargetWidth, kTargetHeight, 4,
                  fmt::format("{}/stencil_source_{:08X}.raw", stencil_root_, dest_address));
      }
      CaptureStencil(source->image, source_x, source_y, width, height);
      dk::ImageView stencil_view{surface->stencil_image};
      Blit(stencil_scratch_descriptor_, stencil_view, surface->width, surface->height, source_x, source_y, dest_x, dest_y,
           width, height, 0);
    }
    // The game reads what it resolves back from memory: pieces of a Sim's atlas while building the next piece,
    // and finished atlases it moves with the CPU. Anything resolved every frame is left out: only the GPU reads it.
    // Steady means resolved on several frames in a row. Repeats within one frame are pieces of one build (an atlas
    // gets several), and each must reach memory, or a CPU copy of the finished atlas holds stale pieces.
    ResolveRun& run = resolve_last_[dest_address];
    if (run.frame != frame_) {
      // Gaps up to 4 frames still count (sims3_087: resolves every 2-3 frames were written back each time,
      // each a full GPU stop that also ended the frame overlap).
      run.count = run.count && frame_ - run.frame <= 4 ? run.count + 1 : 1;
      run.frame = frame_;
    }
    // Long enough that a run of Sim builds at world load (one per frame into the same atlas) is still written back.
    // Tiny resolves are always written: the ambient light probe (the lot drawn into 16x16 views, resolved to 32x16
    // every frame) is read by the CPU, which builds the ambient cube from it. Without it the cube stayed black and
    // every shaded pixel lost its ambient light (sims3_052-054: black shade outdoors and indoors).
    const bool tiny = uint64_t(width) * height * 4 <= 4096;
    const bool steady = run.count >= 30 && !tiny;
    if (tiny && run.count == 30 && tiny_writebacks_logged_ < 8) {
      ++tiny_writebacks_logged_;
      REXLOG_INFO("[render] small resolve {:08X} ({}x{}) written back every frame", dest_address, width, height);
    }
    // Memory then holds every piece written so far, which a surface assembled here may not.
    // Tiny ones (the light probe, sims3_090: ~40 full GPU stops per 5 s) are copied in this frame's command
    // list and reach memory once the frame is done (about a frame late), with no stop.
    surface->guest_current = plain_rgba && !depth && !steady && surface->width <= 1024 &&
                             (tiny ? DeferWriteBack(surface, dest_x, dest_y, width, height, run.count < 30)
                                   : WriteBack(surface, dest_x, dest_y, width, height, true));
    if (surface->guest_current) {
      WritebackStat& stat = writeback_stats_[dest_address];
      ++stat.count;
      stat.width = width;
      stat.height = height;
      stat.run = run.count;
    }
    // Black shade (sims3_062: no steady small resolve logged): what each tiny resolve holds, now and then.
    if ((tiny || (width <= 512 && height <= 512)) && !depth && probe_logs_ < 120 &&
        (frame_ - probe_logged_frame_[dest_address]) >= 600) {
      probe_logged_frame_[dest_address] = frame_;
      ++probe_logs_;
      double sum[4] = {};
      const size_t texels = surface->guest_current ? scratch_.size() / 4 : 0;
      for (size_t i = 0; i < texels; ++i) {
        for (int c = 0; c < 4; ++c) sum[c] += scratch_[i * 4 + c];
      }
      const double n = texels ? double(texels) * 255.0 : 1.0;
      REXLOG_INFO("[render] tiny resolve {:08X} {}x{} at {},{}: run {}, plain {}, written {}, mean {:.3f} {:.3f} {:.3f} {:.3f}",
                  dest_address, width, height, dest_x, dest_y, run.count, plain_rgba, surface->guest_current, sum[0] / n,
                  sum[1] / n, sum[2] / n, sum[3] / n);
    }
    if (!step_root_.empty() && !depth && steps_saved_ < 160 &&
        surface->width == 512 && surface->height <= 256) {  // green portraits: the Sim composite pieces
      SaveStep(surface);
    }
    // Test runs only (capture/portrait.on): after the game draws a Sim headshot, its 640x360 two-copy render and
    // the 256x256 portrait combined from it are saved as the GPU drew them.
    if (portrait_armed_ && !depth && ((surface->width == 640 && surface->height == 360) ||
                                      (surface->width == 256 && surface->height == 256))) {
      SaveSurface(surface, fmt::format("{}/portrait{}_{}_{:08X}_{}x{}.raw", portrait_root_, portraits_saved_,
                                       3 - portrait_armed_, surface->base, surface->width, surface->height));
      if (--portrait_armed_ == 0) ++portraits_saved_;
    }
    // guest_known stays false: hashing the surface's memory at every resolve (and again at its first fetch each
    // frame) only fed a log line, and cost a share of the render thread.
    surface->checked_frame = frame_;
  }

  // The stencil of a depth target's rectangle into the R8 scratch at the same place, a bit per pass: each pass
  // tests one bit and adds it. A stencil view sampled in a shader gave no marks on the console (sims3_047: the
  // hover outline still filled), so the stencil is only ever used as a stencil here.
  void CaptureStencil(const dk::Image& source, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!stencil_scratch_made_) {
      MakeImage(DkImageFormat_R8_Unorm, kTargetWidth, kTargetHeight, stencil_scratch_memory_, stencil_scratch_);
      dk::ImageView view{stencil_scratch_};
      stencil_scratch_descriptor_ = NewDescriptor(view);
      stencil_scratch_made_ = true;
    }
    commands_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    dk::ImageView colour{stencil_scratch_};
    dk::ImageView depth{source};
    commands_.bindRenderTargets(&colour, &depth);
    commands_.setViewports(0, {DkViewport{0.0f, 0.0f, float(kTargetWidth), float(kTargetHeight), 0.0f, 1.0f}});
    commands_.setScissors(0, {DkScissor{x, y, width, height}});
    commands_.bindColorWriteState(dk::ColorWriteState{}.setMask(0, DkColorMask_RGBA));
    commands_.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f);
    commands_.bindShaders(DkStageFlag_GraphicsMask, {&blit_vertex_, &blit_stencil_fragment_});
    commands_.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
    commands_.bindVtxAttribState({});
    commands_.bindVtxBufferState({});
    commands_.bindColorState(dk::ColorState{}.setBlendEnable(0, true));
    commands_.bindBlendStates(0, {dk::BlendState{}
                                      .setOps(DkBlendOp_Add, DkBlendOp_Add)
                                      .setFactors(DkBlendFactor_One, DkBlendFactor_One, DkBlendFactor_One,
                                                  DkBlendFactor_One)});
    commands_.bindDepthStencilState(dk::DepthStencilState{}
                                        .setDepthTestEnable(false)
                                        .setDepthWriteEnable(false)
                                        .setStencilTestEnable(true)
                                        .setStencilFrontCompareOp(DkCompareOp_Equal)
                                        .setStencilFrontPassOp(DkStencilOp_Keep)
                                        .setStencilFrontFailOp(DkStencilOp_Keep)
                                        .setStencilFrontDepthFailOp(DkStencilOp_Keep)
                                        .setStencilBackCompareOp(DkCompareOp_Equal)
                                        .setStencilBackPassOp(DkStencilOp_Keep)
                                        .setStencilBackFailOp(DkStencilOp_Keep)
                                        .setStencilBackDepthFailOp(DkStencilOp_Keep));
    const float data[8] = {-1.0f, 1.0f, 1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t bit = 0; bit < 8; ++bit) {
      Arena pass = Allocate(256, DK_UNIFORM_BUF_ALIGNMENT);
      if (!pass.cpu) break;
      const int32_t mode[4] = {int32_t(bit), 0, 0, 0};
      std::memcpy(pass.cpu, data, sizeof(data));
      std::memcpy(pass.cpu + sizeof(data), mode, sizeof(mode));
      commands_.setStencil(DkFace_FrontAndBack, 0, uint8_t(1u << bit), uint8_t(1u << bit));
      commands_.bindUniformBuffer(DkStage_Vertex, 0, pass.gpu, 256);
      commands_.bindUniformBuffer(DkStage_Fragment, 0, pass.gpu, 256);
      commands_.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);
    }
    commands_.bindColorState(dk::ColorState{});
    commands_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    current_colour_ = current_depth_ = nullptr;
    program_ = Program::kNone;
  }
  dk::UniqueMemBlock stencil_scratch_memory_;
  dk::Image stencil_scratch_;
  uint32_t stencil_scratch_descriptor_ = 0;
  bool stencil_scratch_made_ = false;

  // Test runs only (capture/stencil.on): one frame with the hover highlight saves the depth target at its
  // resolve, the R8 stencil copy and the depth target after the reload, raw.
  void SaveImage(const dk::Image& image, uint32_t width, uint32_t height, uint32_t bpp, const std::string& path) {
    const uint32_t bytes = width * height * bpp;
    if (bytes > kStagingMemory) return;
    FlushFrame();
    dk::ImageView view{image};
    commands_.copyImageToBuffer(view, DkImageRect{0, 0, 0, width, height, 1},
                                DkCopyBuf{staging_memory_.getGpuAddr(), 0, 0});
    queue_.submitCommands(commands_.finishList());
    queue_.waitIdle();
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(staging_memory_.getCpuAddr(), 1, bytes, f);
      std::fclose(f);
    }
    current_colour_ = current_depth_ = nullptr;
    program_ = Program::kNone;
    REXLOG_INFO("[render] saved {}", path);
  }
  void SetStencilRoot(std::string root) { stencil_root_ = std::move(root); }
  void SetIconRoot(std::string root) { icon_root_ = std::move(root); }
  const std::string& icon_root() const { return icon_root_; }
  std::string icon_root_;
  uint32_t icon_dumps_ = 0, atlas_dumps_ = 0, sim_atlas_dumps_ = 0, small_logs_ = 0, sim_atlas_logs_ = 0;
  std::string stencil_root_;
  uint32_t stencil_dumps_ = 0;
  uint64_t highlight_frame_ = ~0ull;

  // Writes a resolved depth surface into a depth target; see blit_depth.frag.glsl.
  bool RestoreDepth(uint32_t surface_address, uint32_t depth_key) {
    Surface* surface = FindSurface(surface_address);
    if (!surface || !surface->depth) return false;
    Target* depth = FindTarget(depths_, depth_key, true);
    static constexpr uint32_t kIdentity[4] = {0, 1, 2, 3};
    uint32_t source = 0;
    SurfaceTexture(surface->base, kIdentity, &source);
    Arena block = Allocate(256, DK_UNIFORM_BUF_ALIGNMENT);
    if (!block.cpu) return false;
    const float data[8] = {-1.0f, 1.0f, float(surface->width) * 2.0f / float(kTargetWidth) - 1.0f,
                           1.0f - float(surface->height) * 2.0f / float(kTargetHeight),
                           0.0f, 0.0f, float(surface->width), float(surface->height)};
    std::memcpy(block.cpu, data, sizeof(data));
    commands_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    dk::ImageView depth_view{depth->image};
    commands_.bindRenderTargets({}, &depth_view);
    commands_.setViewports(0, {DkViewport{0.0f, 0.0f, float(kTargetWidth), float(kTargetHeight), 0.0f, 1.0f}});
    commands_.setScissors(0, {DkScissor{0, 0, kTargetWidth, kTargetHeight}});
    commands_.bindShaders(DkStageFlag_GraphicsMask, {&blit_vertex_, &blit_depth_fragment_});
    commands_.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
    commands_.bindVtxAttribState({});
    commands_.bindVtxBufferState({});
    commands_.bindColorState(dk::ColorState{});
    commands_.bindDepthStencilState(dk::DepthStencilState{}
                                        .setDepthTestEnable(true)
                                        .setDepthWriteEnable(true)
                                        .setDepthCompareOp(DkCompareOp_Always));
    commands_.bindUniformBuffer(DkStage_Vertex, 0, block.gpu, 256);
    commands_.bindTextures(DkStage_Fragment, 0, {dkMakeTextureHandle(source, 0)});
    pixel_bound_[0] = ~0u;
    commands_.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);
    // Then the stencil, one bit per pass: Direct3D's reload writes whole depth words, stencil included, and the
    // game keeps stencil marks across it (the hover outline marks the object before the reload and tests after).
    if (surface->has_stencil && kRestoreStencil) {
      commands_.setScissors(0, {DkScissor{0, 0, std::min(surface->width, kTargetWidth),
                                          std::min(surface->height, kTargetHeight)}});
      commands_.clearDepthStencil(false, 0.0f, 0xFF, 0);
      commands_.bindShaders(DkStageFlag_GraphicsMask, {&blit_vertex_, &stencil_bit_fragment_});
      commands_.bindColorWriteState(dk::ColorWriteState{}.setMask(0, 0));
      commands_.bindTextures(DkStage_Fragment, 0, {dkMakeTextureHandle(surface->views[kStencilView], 0)});
      for (uint32_t bit = 0; bit < 8; ++bit) {
        Arena pass = Allocate(256, DK_UNIFORM_BUF_ALIGNMENT);
        if (!pass.cpu) break;
        const int32_t mode[4] = {int32_t(bit), 0, 0, 0};
        std::memcpy(pass.cpu, data, sizeof(data));
        std::memcpy(pass.cpu + sizeof(data), mode, sizeof(mode));
        commands_.bindDepthStencilState(dk::DepthStencilState{}
                                            .setDepthTestEnable(false)
                                            .setDepthWriteEnable(false)
                                            .setStencilTestEnable(true)
                                            .setStencilFrontCompareOp(DkCompareOp_Always)
                                            .setStencilFrontPassOp(DkStencilOp_Replace)
                                            .setStencilFrontFailOp(DkStencilOp_Keep)
                                            .setStencilFrontDepthFailOp(DkStencilOp_Replace)
                                            .setStencilBackCompareOp(DkCompareOp_Always)
                                            .setStencilBackPassOp(DkStencilOp_Replace)
                                            .setStencilBackFailOp(DkStencilOp_Keep)
                                            .setStencilBackDepthFailOp(DkStencilOp_Replace));
        commands_.setStencil(DkFace_FrontAndBack, uint8_t(1u << bit), 0xFF, 0xFF);
        commands_.bindUniformBuffer(DkStage_Vertex, 0, pass.gpu, 256);
        commands_.bindUniformBuffer(DkStage_Fragment, 0, pass.gpu, 256);
        commands_.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);
      }
      if (highlight_frame_ == frame_ && !stencil_root_.empty() && stencil_dumps_ == 0) {
        ++stencil_dumps_;
        SaveImage(surface->stencil_image, surface->width, surface->height, 1,
                  fmt::format("{}/stencil_copy_{:08X}.raw", stencil_root_, surface->base));
        SaveImage(depth->image, kTargetWidth, kTargetHeight, 4, fmt::format("{}/stencil_reloaded.raw", stencil_root_));
      }
      if (!stencil_restore_logged_) {
        stencil_restore_logged_ = true;
        REXLOG_INFO("[render] stencil restored with depth from {:08X}, {}x{}", surface->base, surface->width,
                    surface->height);
      }
    }
    commands_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    current_colour_ = current_depth_ = nullptr;
    program_ = Program::kNone;
    return true;
  }
  bool stencil_restore_logged_ = false;
  uint32_t tiny_writebacks_logged_ = 0, probe_logs_ = 0, cube_logs_ = 0;
  std::unordered_map<uint32_t, uint64_t> probe_logged_frame_;

  // Copies a resolved colour surface back into a colour target, undoing the swap of its resolve.
  bool RestoreColour(uint32_t surface_address, uint32_t colour_key) {
    Surface* surface = FindSurface(surface_address);
    if (!surface || surface->depth) return false;
    Target* target = FindTarget(colours_, colour_key, false);
    static constexpr uint32_t kIdentity[4] = {0, 1, 2, 3};
    uint32_t source = 0;
    SurfaceTexture(surface->base, kIdentity, &source);
    dk::ImageView view{target->image};
    Blit(source, view, kTargetWidth, kTargetHeight, 0, 0, 0, 0, std::min(surface->width, kTargetWidth),
         std::min(surface->height, kTargetHeight), surface->swapped ? 2 : 0);
    return true;
  }

  // Saves the larger colour targets and surfaces. The frame must be finished. GPU memory goes through
  // the heap first: the file system can stall on memory mapped for the GPU.

  void Dump(const std::string& prefix, bool main_only = false) {
    const auto save = [&](const dk::Image& image, uint32_t width, uint32_t height, const std::string& name) {
      const uint32_t bytes = width * height * 4;
      if (bytes > kStagingMemory) return;
      std::vector<uint8_t> copy(bytes);
      commands_.clear();
      dk::ImageView view{image};
      commands_.copyImageToBuffer(view, DkImageRect{0, 0, 0, width, height, 1},
                                  DkCopyBuf{staging_memory_.getGpuAddr(), 0, 0});
      queue_.submitCommands(commands_.finishList());
      queue_.waitIdle();
      const std::string path = fmt::format("{}_{}.raw", prefix, name);
      std::memcpy(copy.data(), staging_memory_.getCpuAddr(), bytes);
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(copy.data(), 1, bytes, f);
        std::fclose(f);
      }
    };
    for (const auto& target : colours_) {
      if ((target->key & 0xFFFF) < (main_only ? 1280u : 640u)) continue;
      save(target->image, kTargetWidth, kTargetHeight,
           fmt::format("target{:03X}_{}_{}x{}", target->key >> 16, target->key & 0xFFFF, kTargetWidth, kTargetHeight));
    }
    for (const auto& surface : surfaces_) {
      if (surface->width < (main_only ? 1280u : 640u) || (main_only && surface->depth)) continue;
      save(surface->image, surface->width, surface->height,
           fmt::format("surface{:08X}_{}x{}{}", surface->base, surface->width, surface->height,
                       surface->depth ? "_f" : ""));
    }
    REXLOG_INFO("[render] dumped {} targets and the larger of {} surfaces to {}", colours_.size(), surfaces_.size(), prefix);
  }

  // A surface resolved earlier, seen through the fetch constant's swizzle.
  // What a surface was resolved over in guest memory. The console's resolve writes that memory, so once the
  // CPU has written there the memory is the newest picture and the surface is out of date.
  void SetGuestMemory(std::function<const uint8_t*(uint32_t, uint32_t)> guest) { guest_ = std::move(guest); }
  void SetStepRoot(std::string root) { step_root_ = std::move(root); }
  void SetPortraitRoot(std::string root) { portrait_root_ = std::move(root); }
  bool PortraitDumps() const { return !portrait_root_.empty(); }
  // Called on each draw of a headshot's second copy; the next two headshot-sized resolves are saved.
  void ArmPortrait() {
    if (portrait_root_.empty() || portrait_armed_ || portraits_saved_ >= 8 || portrait_frame_ == frame_) return;
    portrait_frame_ = frame_;
    portrait_armed_ = 2;
  }

  bool SurfaceTexture(uint32_t address, const uint32_t* swizzle, uint32_t* out_id, uint32_t signs = 0,
                      uint32_t fetch_width = 0, uint32_t fetch_height = 0, uint32_t fetch_format = 6) {
    // A surface is 32 bits a texel. A fetch in any other format means the game has reused the address.
    const auto report_refused = [&](const char* reason) {
      const uint64_t key = (uint64_t(address) << 32) | (fetch_format << 26) | (fetch_width << 13) | fetch_height;
      if (refused_fetches_.size() >= 60 || !refused_fetches_.insert(key).second) return;
      for (const auto& known : surfaces_) {
        if (known->base == address) {
          REXLOG_INFO("[render] fetch refused ({}): {:08X} format {} {}x{}, surface {}x{}", reason, address, fetch_format,
                      fetch_width, fetch_height, known->width, known->height);
        }
      }
    };
    if (fetch_format != 6 && fetch_format != 22 && fetch_format != 23 && fetch_format != 36 && fetch_format != 50) {
      report_refused("format");
      return false;
    }
    Surface* surface = nullptr;
    for (const auto& known : surfaces_) {
      if (known->base != address) continue;
      if (known->width == fetch_width && known->height == fetch_height) {
        surface = known.get();
        break;
      }
      if (!surface && (!fetch_width || (known->width == fetch_width && known->height >= fetch_height))) {
        surface = known.get();
      }
    }
    if (!surface) {
      report_refused("size");
      return false;
    }
    if (fetch_width && surface->guest_known && surface->checked_frame != frame_) {
      surface->checked_frame = frame_;
      // Reported, not acted on: replacing a surface here left Sims black, so the game must touch these itself.
      if (GuestHash(surface) != surface->guest_hash) {
        surface->guest_known = false;
        if (++overwrites_logged_ <= 40) {
          REXLOG_INFO("[render] guest memory of surface {:08X} ({}x{}) changed after its resolve, frame {}",
                      surface->base, surface->width, surface->height, frame_);
        }
      }
    }
    const uint32_t kind = surface->depth ? 0 : GammaFetch(signs) ? 1 : SignedFetch(signs) ? 2 : 0;
    const uint32_t key = swizzle[0] | (swizzle[1] << 3) | (swizzle[2] << 6) | (swizzle[3] << 9);
    *out_id = ViewOf(surface, key, kind);
    return true;
  }

  uint32_t cube_descriptor() const { return cube_descriptor_; }

  bool HasSurface(uint32_t address) const {
    for (const auto& known : surfaces_) {
      if (address >= known->base && address - known->base < known->bytes) return true;
    }
    return false;
  }

  std::string Describe(uint32_t address) const {
    for (const auto& known : surfaces_) {
      if (address > known->base && address - known->base < known->bytes) {
        return fmt::format("inside surface {:08X}, {}x{}, at offset {:X}", known->base, known->width, known->height,
                           address - known->base);
      }
    }
    for (auto it = gone_.rbegin(); it != gone_.rend(); ++it) {
      if (it->first == address) return "its surface was replaced at frame " + it->second;
    }
    return "never resolved here";
  }

  // Stands in for textures in formats not handled yet.
  Texture WhiteTexture() {
    static const uint8_t kWhite[4] = {255, 255, 255, 255};
    TextureRequest request{};
    request.key = {~0u, ~0u, ~0u, ~0u};
    request.guest = kWhite;
    request.guest_bytes = 4;
    request.format = 6;
    request.width = request.height = request.pitch = 1;
    request.swizzle[1] = 1;
    request.swizzle[2] = 2;
    request.swizzle[3] = 3;
    return FindTexture(request);
  }

  void DrawGeneric(const GenericDraw& d) {
    ++frame_draws_;
    if ((d.depth_control & 0xFFFF) == 0x8733) highlight_frame_ = frame_;  // the hover outline's marking pass
    uint64_t& seen = pair_frame_[d.vertex_hash ^ (d.pixel_hash * 31)];
    if (seen + 300 < frame_ && rare_draws_.size() < 900) {
      rare_draws_ += fmt::format(" vs {:016x} ps {:016x} prim {} count {};", d.vertex_hash, d.pixel_hash,
                                 int(d.primitive), d.count);
    }
    seen = frame_;
    if (program_ != Program::kGeneric) {
      program_ = Program::kGeneric;
      commands_.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
      commands_.bindVtxAttribState({});
      commands_.bindVtxBufferState({});
      raster_state_ = 0;
    }
    if (const uint32_t state = uint32_t(d.bias_on) | (d.cull << 1); state != raster_state_) {
      raster_state_ = state;
      static constexpr DkFace kCull[4] = {DkFace_None, DkFace_Front, DkFace_Back, DkFace_FrontAndBack};
      // FACE 0 is counter-clockwise front as seen on screen (as Xenia's FrontCounterClockwise = !face); deko3d's
      // upper-left origin keeps its winding as seen on screen too.
      commands_.bindRasterizerState(dk::RasterizerState{}
                                        .setCullMode(kCull[d.cull & 3])
                                        .setFrontFace((d.cull & 4) ? DkFrontFace_CW : DkFrontFace_CCW)
                                        .setDepthBiasEnable(d.bias_on));
    }
    if (d.bias_on && (d.bias_constant != bias_constant_ || d.bias_slope != bias_slope_)) {
      bias_constant_ = d.bias_constant;
      bias_slope_ = d.bias_slope;
      commands_.setDepthBias(d.bias_constant, 0.0f, d.bias_slope);
    }
    commands_.bindShaders(DkStageFlag_GraphicsMask, {d.vertex, d.pixel});
    commands_.bindUniformBuffer(DkStage_Vertex, 0, d.vertex_constants, 4096);
    commands_.bindUniformBuffer(DkStage_Vertex, 1, d.state, 256);
    commands_.bindUniformBuffer(DkStage_Fragment, 0, d.pixel_constants, 4096);
    commands_.bindUniformBuffer(DkStage_Fragment, 1, d.state, 256);
    for (uint32_t i = 0; i < d.fetch_count; ++i) {
      commands_.bindStorageBuffer(DkStage_Vertex, i, d.fetch[i].addr, d.fetch[i].size);
    }
    for (uint32_t i = 0; i < d.vertex_texture_count; ++i) {
      commands_.bindTextures(DkStage_Vertex, d.vertex_slot[i], {d.vertex_handle[i]});
      used_descriptors_.set(d.vertex_handle[i] & 0xFFFFF);
    }
    for (uint32_t i = 0; i < d.pixel_texture_count; ++i) {
      if (pixel_bound_[d.pixel_slot[i]] != d.pixel_handle[i]) {
        pixel_bound_[d.pixel_slot[i]] = d.pixel_handle[i];
        commands_.bindTextures(DkStage_Fragment, d.pixel_slot[i], {d.pixel_handle[i]});
      }
      used_descriptors_.set(d.pixel_handle[i] & 0xFFFFF);
    }
    BindBlend(d.blend, d.alpha_control);
    if (d.alpha_control & 8) commands_.setAlphaRef(d.alpha_ref);
    if (d.primitive == DkPrimitive_Points) commands_.setPointSize(d.point_size);
    commands_.bindColorWriteState(dk::ColorWriteState{}.setMask(0, d.write_mask & DkColorMask_RGBA));
    BindDepthStencil(d.depth_control, d.stencil_word, d.stencil_back);
    commands_.setScissors(0, {d.scissor});
    if (d.indexed) {
      commands_.bindIdxBuffer(d.index32 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16, d.indices);
      commands_.drawIndexed(d.primitive, d.count, 1, 0, 0, 0);
    } else {
      commands_.draw(d.primitive, d.count, 1, 0, 0);
    }
    if (++chunk_draws_ >= kChunkDraws) SubmitChunk();
  }

  // Hands what is recorded so far to the GPU without waiting, so the GPU draws while the next draws are
  // translated (the whole frame used to wait for Present, then the CPU waited for the GPU). Every chunk ends
  // with a full barrier: an upload recorded for the next chunk must not overwrite a texture this one still reads.
  void SubmitChunk() {
    ProfileScope scope(kProfSubmit);
    chunk_draws_ = 0;
    if (!open_) return;
    commands_.barrier(DkBarrier_Full, 0);
    if (frame_used_ > flushed_used_) {
      frame_memory_.flushCpuCache(uint32_t(flushed_used_), uint32_t(frame_used_ - flushed_used_));
      flushed_used_ = frame_used_;
    }
    if (uploaded_) {
      uploads_.barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
      queue_.submitCommands(uploads_.finishList());
      uploaded_ = false;
    }
    queue_.submitCommands(commands_.finishList());
    queue_.flush();
    ++chunks_;
    // Descriptors written since are seen by what follows.
    commands_.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
  }

  // Starts the frame on first use: a green pulse stays behind everything, so gaps are easy to see.
  void Begin(float r, float g, float b) {
    if (open_) return;
    // The slot's last frame must be done before its memory is written again; then what it retired can go.
    slot_index_ = (slot_index_ + 1) % kFrameSlots;
    if (fence_pending_[slot_index_]) {
      ProfileScope scope(kProfFence);
      frame_fences_[slot_index_].wait();
      fence_pending_[slot_index_] = false;
    }
    if (queue_.isInErrorState()) RecoverQueue();
    ApplyWriteBacks(slot_index_);
    readback_used_ = 0;
    ReleaseRetired(slot_index_);
    const bool wipe_textures = texture_used_ > kTexturePool / 8 * 7 || next_descriptor_ + 64 >= persistent_descriptor_;
    if (wipe_textures || vertex_cache_full_) {  // the frame still in flight may use what is about to be reused
      queue_.waitIdle();
      for (uint32_t i = 0; i < kFrameSlots; ++i) {
        fence_pending_[i] = false;
        ApplyWriteBacks(i);
        ReleaseRetired(i);
      }
    }
    if (wipe_textures) {
      REXLOG_INFO("[render] texture cache wiped: {} textures, {:.1f} MB, descriptors {} of {}", textures_.size(),
                  double(texture_used_) / 1048576.0, next_descriptor_, persistent_descriptor_);
      textures_.clear();
      texture_used_ = 0;
      next_descriptor_ = 0;
    }
    if (vertex_cache_full_) {  // nothing from the last frame is running now
      REXLOG_INFO("[render] vertex cache wiped at {:.1f} MB", double(vertex_cache_used_) / 1048576.0);
      vertex_cache_full_ = false;
      vertex_cache_used_ = 0;
      ++vertex_cache_generation_;
    }
    ++frame_;
    const uint32_t command_base = (kCommandMemory * 2 + kCommandExtra) * slot_index_;
    commands_.clear();
    commands_.addMemory(command_memory_, command_base, kCommandMemory);
    command_extra_used_ = 0;
    uploads_.clear();
    uploads_.addMemory(command_memory_, command_base + kCommandMemory, kCommandMemory);
    vertices_ = reinterpret_cast<UiVertex*>(static_cast<uint8_t*>(ui_vertex_memory_.getCpuAddr()) +
                                            slot_index_ * kVertexMemory);
    vertex_used_ = 0;
    staging_base_ = size_t(kStagingMemory) * slot_index_;
    staging_used_ = staging_base_;
    frame_base_ = size_t(kFrameMemory) * slot_index_;
    frame_used_ = frame_base_;
    flushed_used_ = frame_base_;
    chunk_draws_ = 0;
    uploaded_ = false;
    pixel_bound_.fill(~0u);
    raster_state_ = 0;
    bias_constant_ = bias_slope_ = -1e30f;
    if (shaders_fresh_) {  // the code cache has to learn about the shaders loaded at startup
      shaders_fresh_ = false;
      uploads_.barrier(DkBarrier_None, DkInvalidateFlags_Shader);
      uploaded_ = true;
    }
    blend_valid_ = false;
    current_colour_ = current_depth_ = nullptr;
    program_ = Program::kNone;
    commands_.bindSamplerDescriptorSet(data_memory_.getGpuAddr() + kVertexMemory + 0x400, 2);
    commands_.bindImageDescriptorSet(data_memory_.getGpuAddr() + kVertexMemory + 0x1000, kDescriptorSlots);
    // The last frame may still be drawing: its reads of targets and textures end before this frame writes them.
    commands_.barrier(DkBarrier_Full, DkInvalidateFlags_Descriptors);
    uploads_.barrier(DkBarrier_Full, 0);
    open_ = true;
  }

  void UseInterfaceProgram() {
    if (program_ == Program::kInterface) return;
    program_ = Program::kInterface;
    commands_.bindShaders(DkStageFlag_GraphicsMask, {&vertex_shader_, &fragment_shader_});
    commands_.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
    commands_.bindVtxAttribState({
        DkVtxAttribState{0, 0, offsetof(UiVertex, x), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        DkVtxAttribState{0, 0, offsetof(UiVertex, u), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        DkVtxAttribState{0, 0, offsetof(UiVertex, c0), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
        DkVtxAttribState{0, 0, offsetof(UiVertex, c1), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
    });
    commands_.bindVtxBufferState({DkVtxBufferState{sizeof(UiVertex), 0}});
    commands_.bindVtxBuffer(0, ui_vertex_memory_.getGpuAddr() + slot_index_ * kVertexMemory, kVertexMemory);
    commands_.bindUniformBuffer(DkStage_Fragment, 0, uniform_address_, kUniformMemory);
  }

  UiVertex* Reserve(uint32_t count, uint32_t* first) {
    if (vertex_used_ + count > kVertexMemory / sizeof(UiVertex)) return nullptr;
    *first = vertex_used_;
    vertex_used_ += count;
    return vertices_ + *first;
  }

  // The CPU writes a Sim's 512x256 DXT5 composite a piece at a time: sampled hashing missed the last pieces
  // (green portraits, sims3_071-072). Hashing every tiled texture up to 256 KB whole for it cost about 600 ms
  // per 5 s of the render thread (sims3_085), so only the composites are hashed whole now.
  static bool SimComposite(const TextureRequest& r) { return r.format == 20 && r.width == 512 && r.height == 256; }

  Texture FindTexture(const TextureRequest& request) {
    auto it = textures_.find(request.key);
    if (it == textures_.end()) {
      if (!CreateTexture(request)) return Texture{};
      it = textures_.find(request.key);
    }
    Entry& entry = it->second;
    uint64_t& checked = request.full_hash ? entry.full_frame : entry.frame;
    const uint64_t stamp = frame_ + (uint64_t(write_epoch_) << 40);  // a write back can change a texture mid-frame
    // A texture unchanged for 60 checks is looked at every 4th frame, spread by its descriptor, unless a write-back
    // happened since: hashing every texture every frame cost about a tenth of the render thread.
    // Small textures never rest: the 4x4 palette a Sim's composite is tinted from (skin tone, outfit colours) sits
    // unchanged for minutes, then the CPU rewrites it just before a build. Resting, the build used the old palette
    // 3 times in 4 (green or teal skin and outfits in portraits and on Sims, sims3_070-073).
    const bool rest = kTextureRest && request.guest_bytes > kRestMinBytes && entry.stable >= 60 &&
                      entry.epoch == write_epoch_ && ((frame_ + entry.texture.id) & 3) != 0;
    if (checked != stamp && !rest) {
      checked = stamp;
      entry.epoch = write_epoch_;
      uint64_t& known = request.full_hash ? entry.full_hash : entry.hash;
      // Linear textures are the ones the CPU writes (lightmaps, compressed composites): hash all of them. So are
      // tiled ones up to 256 KB: the CPU writes a Sim's 512x256 DXT5 composite (headshots) a piece at a time, and
      // 8 bytes in 520 missed the last pieces (green portraits, noise on clothes after Randomize, sims3_071-072).
      uint64_t hash;
      {
        ProfileScope scope(kProfHash);
        TextureStat& stat = g_texture_stats[(request.key[1] >> 12) << 12];
        ++stat.checks;
        stat.format = request.format;
        stat.width = request.width;
        stat.height = request.height;
        stat.tiled = request.tiled;
        stat.full = request.full_hash;
        stat.hashed += request.guest_bytes;
        hash = HashMemory(request.guest, request.guest_bytes,
                          kSampledHash && !request.full_hash && request.guest_bytes > kFullHashBytes &&
                              (request.tiled || request.guest_bytes > (1u << 20)) && !SimComposite(request));
        g_hash_bytes += request.guest_bytes;
      }
      if (hash != known) {
        ProfileScope scope(kProfUpload);
        entry.stable = 0;
        ++g_uploads;
        g_upload_bytes += request.guest_bytes;
        TextureStat& stat = g_texture_stats[(request.key[1] >> 12) << 12];
        ++stat.uploads;
        stat.uploaded += request.guest_bytes;
        if (Upload(request, entry)) known = hash;
      } else {
        ++entry.stable;
      }
    }
    return entry.texture;
  }

  // RB_BLEND_RED to RB_BLEND_ALPHA, as raw register words.
  void SetBlendConstants(const uint32_t* words) {
    if (blend_valid_ && std::memcmp(blend_words_, words, sizeof(blend_words_)) == 0) return;
    std::memcpy(blend_words_, words, sizeof(blend_words_));
    blend_valid_ = true;
    commands_.setBlendConst(std::bit_cast<float>(words[0]), std::bit_cast<float>(words[1]),
                            std::bit_cast<float>(words[2]), std::bit_cast<float>(words[3]));
  }

  void Draw(const UiUniforms& uniforms, uint32_t texture_id, bool use_texture, uint32_t first, uint32_t count,
            uint32_t blend, uint32_t write_mask, const DkScissor& scissor, uint32_t depth_control,
            uint32_t stencil_word, uint32_t alpha_control = 0, float alpha_ref = 0.0f) {
    UseInterfaceProgram();
    BindDepthStencil(depth_control & ~uint32_t(6), stencil_word);  // the interface tests no depth
    commands_.pushConstants(uniform_address_, kUniformMemory, 0, sizeof(uniforms), &uniforms);
    if (use_texture) {
      commands_.bindTextures(DkStage_Fragment, 0, {dkMakeTextureHandle(texture_id, 0)});
      pixel_bound_[0] = ~0u;
      used_descriptors_.set(texture_id);
    }
    // The alpha test too: text glow cells keep faint alpha in their padding, cut by the test (box behind labels).
    BindBlend(blend, alpha_control);
    if (alpha_control & 8) commands_.setAlphaRef(alpha_ref);
    commands_.bindColorWriteState(dk::ColorWriteState{}.setMask(0, write_mask & DkColorMask_RGBA));
    commands_.setScissors(0, {scissor});
    commands_.draw(DkPrimitive_Triangles, count, 1, first, 0);
  }

  // The picture is whatever the game resolved to the front buffer, or the main target if it did not.
  void Present(uint32_t front_address) {
    uint32_t source = 0;
    Surface* front = FindSurface(front_address);
    if (front && !front->depth) {
      static constexpr uint32_t kIdentity[4] = {0, 1, 2, 3};
      SurfaceTexture(front->base, kIdentity, &source);
    } else {
      source = FindTarget(colours_, kWidth, false)->descriptor;
    }
    const bool swapped = front && front->swapped;
    if (frame_used_ > flushed_used_) {
      frame_memory_.flushCpuCache(uint32_t(flushed_used_), uint32_t(frame_used_ - flushed_used_));
      flushed_used_ = frame_used_;
    }
    if (uploaded_) {
      uploads_.barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
      queue_.submitCommands(uploads_.finishList());
    }
    // No wait here: the game goes on with the next frame while the GPU draws this one. Begin waits for this
    // slot's fence before its memory is used again.
    queue_.submitCommands(commands_.finishList());
    // The image is taken only for a good frame: one given to a failed queue never comes back.
    if (queue_.isInErrorState()) {
      queue_.waitIdle();
      RecoverQueue();
    } else {
      const size_t blit_start = frame_used_;
      slot_ = queue_.acquireImage(swapchain_);
      dk::ImageView target{images_[slot_]};
      Blit(source, target, kWidth, kHeight, 0, 0, 0, 0, kWidth, kHeight, swapped ? 2 : 0);
      if (frame_used_ > blit_start) {
        frame_memory_.flushCpuCache(uint32_t(blit_start), uint32_t(frame_used_ - blit_start));
      }
      queue_.submitCommands(commands_.finishList());
      queue_.signalFence(frame_fences_[slot_index_], true);
      fence_pending_[slot_index_] = true;
      clocks::Pace();
      queue_.presentImage(swapchain_, slot_);
    }
    rare_draws_.clear();
    frame_draws_ = 0;
    used_descriptors_.reset();
    // Surfaces retired this frame may still be read by it on the GPU: freed when the slot comes round again.
    for (auto& surface : retired_) slot_retired_[slot_index_].push_back(std::move(surface));
    retired_.clear();
    open_ = false;
  }

  void ReleaseRetired(uint32_t slot) {
    for (const auto& surface : slot_retired_[slot]) {
      for (const auto& [swizzle, id] : surface->views) {
        dk::ImageView safe_view{safe_image_};
        image_descriptors_[id].initialize(safe_view);
        free_descriptors_.push_back(id);
      }
    }
    slot_retired_[slot].clear();
  }

  bool TakeWroteBack() {
    const bool wrote = wrote_back_;
    wrote_back_ = false;
    return wrote;
  }

  uint64_t texture_memory_used() const { return texture_used_; }
  uint32_t atlas_events() const { return atlas_events_; }
  size_t texture_count() const { return textures_.size(); }
  size_t surface_count() const { return surfaces_.size(); }
  size_t target_count() const { return colours_.size() + depths_.size(); }
  size_t frame_memory_used() const { return frame_used_ - frame_base_; }
  // Chunks submitted mid-frame and full waits for the GPU (write-backs, step saves) since the last call.
  std::pair<uint32_t, uint32_t> TakeSubmitCounts() {
    const std::pair<uint32_t, uint32_t> counts{chunks_, flushes_};
    chunks_ = flushes_ = 0;
    return counts;
  }

  void Destroy() {
    if (queue_) queue_.waitIdle();
    textures_.clear();
    colours_.clear();
    depths_.clear();
    surfaces_.clear();
    retired_.clear();
    for (auto& list : slot_retired_) list.clear();
    cube_memory_ = nullptr;
    safe_memory_ = nullptr;
    frame_memory_ = nullptr;
    xenos_memory_ = nullptr;
    commands_ = nullptr;
    uploads_ = nullptr;
    command_memory_ = nullptr;
    data_memory_ = nullptr;
    ui_vertex_memory_ = nullptr;
    readback_memory_ = nullptr;
    texture_memory_ = nullptr;
    staging_memory_ = nullptr;
    shader_memory_ = nullptr;
    queue_ = nullptr;
    swapchain_ = nullptr;
    clocks::Stop();
    image_memory_ = nullptr;
    device_ = nullptr;
  }

 private:
  struct Entry {
    Texture texture;
    dk::Image image;
    uint32_t aligned_width = 0, aligned_height = 0;
    uint64_t hash = 0, frame = 0, full_hash = 0, full_frame = 0;
    uint32_t stable = 0;   // checks in a row that found the memory unchanged
    uint32_t epoch = ~0u;  // write_epoch_ at the last check
    bool decoded = false;  // a small compressed texture kept as RGBA8 (kDecodeSmallBc)
    uint32_t mip_levels = 1;
  };

  // A fixed command buffer has nothing to fall back on when it fills, and that stops the program.
  static void AddCommandMemory(void* user, DkCmdBuf commands, size_t minimum) {
    auto* self = static_cast<Deko3dPresenter*>(user);
    const uint32_t size = std::max<uint32_t>(uint32_t(minimum + 0xFFF) & ~0xFFFu, 4u << 20);
    if (self->command_extra_used_ + size > kCommandExtra) {
      REXLOG_ERROR("[render] out of command memory: {} KB used, {} KB wanted", self->command_extra_used_ >> 10, size >> 10);
      return;
    }
    dkCmdBufAddMemory(commands, self->command_memory_,
                      (kCommandMemory * 2 + kCommandExtra) * self->slot_index_ + kCommandMemory * 2 +
                          self->command_extra_used_,
                      size);
    self->command_extra_used_ += size;
    if (self->command_extra_used_ > self->command_extra_peak_) {
      self->command_extra_peak_ = self->command_extra_used_;
      REXLOG_INFO("[render] command memory grew by {} MB", self->command_extra_peak_ >> 20);
    }
  }

  // A failed channel stays usable for questions. The channel sits 0x18 into deko3d's queue object, as
  // Queue::checkError has it in both builds of the library.
  void ReportChannelError() {
    auto* channel = reinterpret_cast<NvGpuChannel*>(reinterpret_cast<uint8_t*>(DkQueue(queue_)) + 0x18);
    NvNotification note{};
    NvError error{};
    const Result note_result = nvGpuChannelGetErrorNotification(channel, &note);
    const Result error_result = nvGpuChannelGetErrorInfo(channel, &error);
    REXLOG_ERROR("[deko3d] {} (type {}): info {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}; notification "
                 "{}/{}/{}, results {:X} {:X}",
                 error.type == 1   ? "GPU page fault"
                 : error.type == 2 ? "GPU method error"
                 : error.type == 3 ? "GPU rejected a command list"
                 : error.type == 4 ? "GPU timeout"
                                   : "no details",
                 error.type, error.info[0], error.info[1], error.info[2], error.info[3], error.info[4], error.info[5],
                 error.info[6], error.info[7], note.info32, note.info16, note.status, note_result, error_result);
    REXLOG_ERROR("[deko3d] memory: commands {:X}, frame {:X}, data {:X}, textures {:X}, staging {:X}, shaders {:X} {:X}",
                 command_memory_.getGpuAddr(), frame_memory_.getGpuAddr(), data_memory_.getGpuAddr(),
                 texture_memory_.getGpuAddr(), staging_memory_.getGpuAddr(), shader_memory_.getGpuAddr(),
                 xenos_memory_.getGpuAddr());
  }

  // The texture units faulted at addresses that belong to nothing, so a descriptor in use must say so.
  void ReportDescriptors() {
    uint64_t lowest = ~uint64_t(0), highest = 0;
    uint32_t bad = 0;
    for (uint32_t id = 0; id < kDescriptorSlots; ++id) {
      if (!used_descriptors_[id]) continue;
      const auto* words = reinterpret_cast<const uint32_t*>(&image_descriptors_[id]);
      const uint64_t address = words[1] | (uint64_t(words[2] & 0xFFFF) << 32);
      lowest = std::min(lowest, address);
      highest = std::max(highest, address);
      const bool gap = id >= next_descriptor_ && id < persistent_descriptor_;
      if ((address >> 32) != 0 && !gap) continue;
      if (++bad > 12) continue;
      std::string owner = gap ? "never assigned" : "unknown";
      for (const auto& [key, entry] : textures_) {
        if (entry.texture.id == id) {
          owner = fmt::format("texture {:08X} {:08X} {:08X} {:08X}, {}x{}", key[0], key[1], key[2], key[3],
                              entry.aligned_width, entry.aligned_height);
        }
      }
      for (const auto& surface : surfaces_) {
        for (const auto& [swizzle, view] : surface->views) {
          if (view == id) owner = fmt::format("surface {:08X}, {}x{}", surface->base, surface->width, surface->height);
        }
      }
      REXLOG_ERROR("[deko3d] bad descriptor {}: address {:X}, words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
                   "{:08X}, {}", id, address, words[0], words[1], words[2], words[3], words[4], words[5], words[6],
                   words[7], owner);
    }
    REXLOG_ERROR("[deko3d] {} descriptors used, {} bad, addresses {:X} to {:X}; ids below {} and from {} are assigned",
                 used_descriptors_.count(), bad, lowest, highest, next_descriptor_, persistent_descriptor_);
  }

  void RecoverQueue() {
    ++queue_errors_;
    if (queue_errors_ <= 8) {
      ReportChannelError();
      ReportDescriptors();
      REXLOG_ERROR("[deko3d] fault {} at frame {}: {} generic draws, {:.1f} MB frame memory, {} KB extra commands, {:.1f} "
                   "MB staging, {} descriptors left, {} textures, {:.1f} MB; rarely used shaders: {}", queue_errors_,
                   frame_, frame_draws_, double(frame_used_) / 1048576.0, command_extra_used_ >> 10,
                   double(staging_used_) / 1048576.0, persistent_descriptor_ - next_descriptor_, textures_.size(),
                   double(texture_used_) / 1048576.0, rare_draws_.empty() ? std::string("none") : rare_draws_);
    }
    if (queue_errors_ == 1) {
      for (const std::string& event : recent_) REXLOG_ERROR("[deko3d]   recent: frame {}", event);
    }
    // The channel is lost but nothing else is: a new queue carries on with the next frame.
    queue_ = nullptr;
    queue_ = dk::QueueMaker{device_}.setFlags(DkQueueFlags_Graphics).create();
  }

  struct Surface;
  static constexpr uint32_t kIdentitySwizzle = 0 | (1 << 3) | (2 << 6) | (3 << 9);

  uint32_t ViewOf(Surface* surface, uint32_t swizzle, uint32_t kind) {
    const uint32_t key = swizzle | (kind << 12);
    const auto found = surface->views.find(key);
    if (found != surface->views.end()) return found->second;
    dk::ImageView view{surface->image};
    view.setSwizzle(ImageSwizzle(swizzle & 7), ImageSwizzle((swizzle >> 3) & 7), ImageSwizzle((swizzle >> 6) & 7),
                    ImageSwizzle((swizzle >> 9) & 7));
    if (kind == 1) view.setFormat(DkImageFormat_RGBA8_Unorm_sRGB);
    if (kind == 2) view.setFormat(DkImageFormat_RGBA8_Snorm);
    const uint32_t id = NewDescriptor(view);
    surface->views[key] = id;
    return id;
  }

  // Everything recorded so far runs now, so a read of the result sees it, and new uploads go ahead of what follows.
  void FlushFrame() {
    ProfileScope scope(kProfFlush);
    ++flushes_;
    if (frame_used_) frame_memory_.flushCpuCache(0, uint32_t(frame_used_));
    if (uploaded_) {
      uploads_.barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
      queue_.submitCommands(uploads_.finishList());
      uploaded_ = false;
    }
    queue_.submitCommands(commands_.finishList());
    queue_.waitIdle();
    uploads_.clear();
    uploads_.addMemory(command_memory_, (kCommandMemory * 2 + kCommandExtra) * slot_index_ + kCommandMemory,
                       kCommandMemory);
    staging_used_ = staging_base_;
  }

  // Puts a resolved rectangle into guest memory the way the console's resolve does: tiled, each word byte-swapped.
  // The reverse of WriteBack: the surface's memory, untiled and byte-swapped, copied into its image.
  bool InitFromGuest(Surface* surface) {
    const uint8_t* guest = guest_ ? guest_(surface->base, surface->bytes) : nullptr;
    const uint32_t bytes = surface->width * surface->height * 4;
    if (!guest || bytes > kStagingMemory) return false;
    if (staging_used_ + bytes > staging_base_ + kStagingMemory) FlushFrame();
    scratch_.resize(bytes);
    uint8_t* out = scratch_.data();
    for (uint32_t row = 0; row < surface->height; ++row) {
      for (uint32_t column = 0; column < surface->width; ++column, out += 4) {
        const uint32_t offset = TiledOffset(column, row, surface->width, 2);
        if (uint64_t(offset) + 4 > surface->bytes) {
          std::memset(out, 0, 4);
          continue;
        }
        out[0] = guest[offset + 3];
        out[1] = guest[offset + 2];
        out[2] = guest[offset + 1];
        out[3] = guest[offset + 0];
      }
    }
    std::memcpy(static_cast<uint8_t*>(staging_memory_.getCpuAddr()) + staging_used_, scratch_.data(), bytes);
    dk::ImageView view{surface->image};
    commands_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    commands_.copyBufferToImage(DkCopyBuf{staging_memory_.getGpuAddr() + staging_used_, 0, 0}, view,
                                DkImageRect{0, 0, 0, surface->width, surface->height, 1});
    commands_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    staging_used_ += (bytes + 0xFF) & ~size_t(0xFF);
    surface->filled_width = surface->width;
    surface->filled_height = surface->height;
    return true;
  }

  void SaveStep(Surface* surface) {
    SaveSurface(surface, fmt::format("{}/step{:03}_{:08X}_f{}_{}x{}.raw", step_root_, steps_saved_++, surface->base,
                                     frame_, surface->width, surface->height));
  }

  void SaveSurface(Surface* surface, const std::string& path) {
    const uint32_t bytes = surface->width * surface->height * 4;
    if (bytes > kStagingMemory) return;
    FlushFrame();
    dk::ImageView view{surface->image};
    commands_.copyImageToBuffer(view, DkImageRect{0, 0, 0, surface->width, surface->height, 1},
                                DkCopyBuf{staging_memory_.getGpuAddr(), 0, 0});
    queue_.submitCommands(commands_.finishList());
    queue_.waitIdle();
    std::vector<uint8_t> copy(bytes);
    std::memcpy(copy.data(), staging_memory_.getCpuAddr(), bytes);
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(copy.data(), 1, bytes, f);
      std::fclose(f);
    }
    current_colour_ = current_depth_ = nullptr;
    program_ = Program::kNone;
  }

 public:
  // Half icons, test runs: a rectangle of a colour target (or of the surface at `address`), read back now. RGBA8,
  // rows packed. Mid-frame, like SaveSurface.
  void* bound_colour() const { return current_colour_; }
  bool ReadTarget(void* handle, uint32_t address, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  std::vector<uint8_t>* out) {
    dk::Image* image = nullptr;
    if (handle) {
      image = &static_cast<Target*>(handle)->image;
    } else if (Surface* surface = FindSurface(address); surface && !surface->depth && x + w <= surface->width &&
                                                        y + h <= surface->height) {
      image = &surface->image;
    }
    const uint32_t bytes = w * h * 4;
    if (!image || bytes > kStagingMemory) return false;
    FlushFrame();
    dk::ImageView view{*image};
    commands_.copyImageToBuffer(view, DkImageRect{x, y, 0, w, h, 1}, DkCopyBuf{staging_memory_.getGpuAddr(), 0, 0});
    queue_.submitCommands(commands_.finishList());
    queue_.waitIdle();
    const auto* data = static_cast<const uint8_t*>(staging_memory_.getCpuAddr());
    out->assign(data, data + bytes);
    current_colour_ = current_depth_ = nullptr;
    program_ = Program::kNone;
    return true;
  }

 private:
  bool HasWrittenPiece(const Surface* surface) const {
    for (const auto& known : surfaces_) {
      if (known.get() != surface && known->guest_current && known->depth == surface->depth &&
          known->width == surface->width && known->base > surface->base &&
          known->base - surface->base < surface->bytes) {
        return true;
      }
    }
    return false;
  }

  struct WritebackStat {
    uint32_t count = 0, width = 0, height = 0, run = 0;
  };
  std::unordered_map<uint32_t, WritebackStat> writeback_stats_;

 public:
  // Speed: what was written back to guest memory in the last 5 s (each one a full GPU stop), most first.
  std::string TakeWritebackStats() {
    std::vector<std::pair<uint32_t, WritebackStat>> rows(writeback_stats_.begin(), writeback_stats_.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
    std::string out;
    for (size_t i = 0; i < rows.size() && i < 10; ++i) {
      out += fmt::format(" {:08X} {}x{}: {} (run {});", rows[i].first, rows[i].second.width, rows[i].second.height,
                         rows[i].second.count, rows[i].second.run);
    }
    writeback_stats_.clear();
    return out;
  }

 private:
  struct PendingWriteBack {
    uint32_t base, surface_width, surface_bytes, x, y, width, height, offset;
    bool bump_epoch;
  };
  std::vector<PendingWriteBack> pending_writebacks_[kFrameSlots];
  uint32_t readback_used_ = 0;
  static constexpr uint32_t kReadbackSlot = 256u << 10;  // per frame slot: 64 light-probe sized copies

  bool DeferWriteBack(Surface* surface, uint32_t x, uint32_t y, uint32_t width, uint32_t height, bool bump_epoch) {
    const uint32_t bytes = width * height * 4;
    if (!bytes || !guest_ || readback_used_ + bytes > kReadbackSlot) {
      return WriteBack(surface, x, y, width, height, bump_epoch);
    }
    const uint32_t offset = slot_index_ * kReadbackSlot + readback_used_;
    readback_used_ = (readback_used_ + bytes + 0xFF) & ~0xFFu;
    dk::ImageView view{surface->image};
    commands_.barrier(DkBarrier_Full, 0);  // the resolve that filled the surface comes first
    commands_.copyImageToBuffer(view, DkImageRect{x, y, 0, width, height, 1},
                                DkCopyBuf{readback_memory_.getGpuAddr() + offset, 0, 0});
    pending_writebacks_[slot_index_].push_back(
        {surface->base, surface->width, surface->bytes, x, y, width, height, offset, bump_epoch});
    return true;
  }

  // After the slot's fence: what its frame copied out goes into guest memory, as WriteBack does.
  void ApplyWriteBacks(uint32_t slot) {
    for (const PendingWriteBack& w : pending_writebacks_[slot]) {
      auto* guest = const_cast<uint8_t*>(guest_(w.base, w.surface_bytes));
      if (!guest) continue;
      const uint8_t* texel = static_cast<const uint8_t*>(readback_memory_.getCpuAddr()) + w.offset;
      for (uint32_t row = 0; row < w.height; ++row) {
        for (uint32_t column = 0; column < w.width; ++column, texel += 4) {
          const uint32_t offset = TiledOffset(w.x + column, w.y + row, w.surface_width, 2);
          if (uint64_t(offset) + 4 > w.surface_bytes) continue;
          uint8_t* out = guest + offset;
          out[0] = texel[3];
          out[1] = texel[2];
          out[2] = texel[1];
          out[3] = texel[0];
        }
      }
      if (w.bump_epoch) {
        ++write_epoch_;
        wrote_back_ = true;
      }
    }
    pending_writebacks_[slot].clear();
  }

  bool WriteBack(Surface* surface, uint32_t x, uint32_t y, uint32_t width, uint32_t height, bool bump_epoch = true) {
    auto* guest = const_cast<uint8_t*>(guest_ ? guest_(surface->base, surface->bytes) : nullptr);
    const uint32_t bytes = width * height * 4;
    if (!guest || !bytes || bytes > kStagingMemory) return false;
    FlushFrame();
    dk::ImageView view{surface->image};
    commands_.copyImageToBuffer(view, DkImageRect{x, y, 0, width, height, 1},
                                DkCopyBuf{staging_memory_.getGpuAddr(), 0, 0});
    queue_.submitCommands(commands_.finishList());
    queue_.waitIdle();
    scratch_.resize(bytes);
    std::memcpy(scratch_.data(), staging_memory_.getCpuAddr(), bytes);
    const uint8_t* texel = scratch_.data();
    for (uint32_t row = 0; row < height; ++row) {
      for (uint32_t column = 0; column < width; ++column, texel += 4) {
        const uint32_t offset = TiledOffset(x + column, y + row, surface->width, 2);
        if (uint64_t(offset) + 4 > surface->bytes) continue;
        uint8_t* out = guest + offset;
        out[0] = texel[3];
        out[1] = texel[2];
        out[2] = texel[1];
        out[3] = texel[0];
      }
    }
    if (bump_epoch) ++write_epoch_;  // tiny per-frame write-backs (the light probe) are read by the CPU only
    if (bump_epoch) wrote_back_ = true;
    current_colour_ = current_depth_ = nullptr;
    program_ = Program::kNone;
    return true;
  }

  uint64_t GuestHash(const Surface* surface) {
    const uint8_t* guest = guest_ ? guest_(surface->base, surface->bytes) : nullptr;
    return guest ? HashMemory(guest, surface->bytes, true) : 0;
  }

  void Retire(Surface* surface) {
    gone_.push_back({surface->base, fmt::format("{}, {}x{}", frame_, surface->width, surface->height)});
    if (gone_.size() > 256) gone_.pop_front();
    for (auto it = surfaces_.begin(); it != surfaces_.end(); ++it) {
      if (it->get() == surface) {
        retired_.push_back(std::move(*it));
        surfaces_.erase(it);
        return;
      }
    }
  }

  // Where a tile-aligned offset into a tiled 32-bit surface lands.
  static bool TileOrigin(uint32_t offset, uint32_t width, uint32_t height, uint32_t* x, uint32_t* y) {
    for (uint32_t row = 0; row < height; row += 32) {
      for (uint32_t column = 0; column < width; column += 32) {
        if (TiledOffset(column, row, width, 2) == offset) {
          *x = column;
          *y = row;
          return true;
        }
      }
    }
    return false;
  }

  // The pieces of an atlas can arrive before its first piece, last address first. They join it when it appears.
  void MergePieces(Surface* atlas) {
    std::vector<Surface*> pieces;
    for (const auto& known : surfaces_) {
      if (known.get() != atlas && known->depth == atlas->depth && known->width == atlas->width &&
          known->base > atlas->base && known->base - atlas->base < atlas->bytes) {
        pieces.push_back(known.get());
      }
    }
    std::sort(pieces.begin(), pieces.end(), [](const Surface* a, const Surface* b) { return a->base < b->base; });
    dk::ImageView view{atlas->image};
    for (Surface* piece : pieces) {
      uint32_t x, y;
      if (!TileOrigin(piece->base - atlas->base, atlas->width, atlas->height, &x, &y)) continue;
      const uint32_t width = std::min(piece->filled_width, atlas->width - x);
      const uint32_t height = std::min(piece->filled_height, atlas->height - y);
      if (width && height) {
        Blit(ViewOf(piece, kIdentitySwizzle, 0), view, atlas->width, atlas->height, 0, 0, x, y, width, height,
             atlas->depth ? 1 : 0);
        atlas->filled_width = std::max(atlas->filled_width, x + width);
        atlas->filled_height = std::max(atlas->filled_height, y + height);
      }
      Remember(fmt::format("piece {:08X} joined {:08X} at {},{}", piece->base, atlas->base, x, y));
      Retire(piece);
    }
  }

  void Remember(std::string event) {
    recent_.push_back(fmt::format("{}: {}", frame_, std::move(event)));
    if (recent_.size() > 32) recent_.pop_front();
  }

  enum class Program { kNone, kInterface, kGeneric };

  struct Target {
    uint32_t key = 0, descriptor = 0;
    uint32_t stencil_descriptor = 0;  // depth targets: a stencil view (unsigned integer in red)
    dk::UniqueMemBlock memory;
    dk::Image image;
  };

  struct Surface {
    uint32_t base = 0, bytes = 0, width = 0, height = 0, filled_width = 0, filled_height = 0;
    bool depth = false;
    bool swapped = false;  // resolved with red and blue exchanged, undone by the fetch swizzle or the display
    bool guest_known = false, guest_current = false;
    uint64_t guest_hash = 0, checked_frame = 0;
    dk::UniqueMemBlock memory;
    dk::Image image;
    std::map<uint32_t, uint32_t> views;  // swizzle to descriptor; kStencilView is the stencil copy's
    // Depth surfaces: the stencil of what was resolved (R8), which Direct3D's EDRAM reload brings back with the
    // depth. Created at the first depth resolve that carries one.
    dk::UniqueMemBlock stencil_memory;
    dk::Image stencil_image;
    bool has_stencil = false;
  };
  static constexpr uint32_t kStencilView = ~0u;

  void MakeImage(DkImageFormat format, uint32_t width, uint32_t height, dk::UniqueMemBlock& memory, dk::Image& image) {
    dk::ImageLayout layout;
    dk::ImageLayoutMaker{device_}
        .setFlags(DkImageFlags_UsageRender)
        .setFormat(format)
        .setDimensions(width, height)
        .initialize(layout);
    const uint32_t size =
        uint32_t((layout.getSize() + DK_MEMBLOCK_ALIGNMENT - 1) & ~uint64_t(DK_MEMBLOCK_ALIGNMENT - 1));
    // Zeroed: a resolve may fill only part of a surface, and the rest is sampled too.
    memory = dk::MemBlockMaker{device_, size}
                 .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image | DkMemBlockFlags_ZeroFillInit)
                 .create();
    image.initialize(layout, memory, 0);
  }

  // Descriptors of targets and surfaces outlive the texture cache, so they count down from the top.
  uint32_t NewDescriptor(const dk::ImageView& view) {
    uint32_t id;
    if (!free_descriptors_.empty()) {
      id = free_descriptors_.back();
      free_descriptors_.pop_back();
    } else {
      id = --persistent_descriptor_;
    }
    image_descriptors_[id].initialize(view);
    uploads_.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
    uploaded_ = true;
    return id;
  }

  Target* FindTarget(std::vector<std::unique_ptr<Target>>& list, uint32_t key, bool depth) {
    for (const auto& target : list) {
      if (target->key == key) return target.get();
    }
    auto target = std::make_unique<Target>();
    target->key = key;
    MakeImage(depth ? DkImageFormat_Z24S8 : DkImageFormat_RGBA8_Unorm, kTargetWidth, kTargetHeight, target->memory,
              target->image);
    dk::ImageView view{target->image};
    target->descriptor = NewDescriptor(view);
    if (depth) {
      dk::ImageView stencil_view{target->image};
      stencil_view.setDsSource(DkDsSource_Stencil);
      target->stencil_descriptor = NewDescriptor(stencil_view);
    }
    REXLOG_INFO("[render] new {} target: EDRAM {:03X}, pitch {}", depth ? "depth" : "colour", key >> 16, key & 0xFFFF);
    list.push_back(std::move(target));
    return list.back().get();
  }

  Surface* FindSurface(uint32_t address) {
    for (const auto& surface : surfaces_) {
      if (address >= surface->base && address - surface->base < surface->bytes) return surface.get();
    }
    return nullptr;
  }

  Surface* CreateSurface(uint32_t address, uint32_t width, uint32_t height, bool depth) {
    auto surface = std::make_unique<Surface>();
    surface->base = address;
    surface->width = std::max(width, 1u);
    surface->height = std::max(height, 1u);
    surface->bytes = surface->width * surface->height * 4;
    surface->depth = depth;
    MakeImage(depth ? DkImageFormat_R32_Float : DkImageFormat_RGBA8_Unorm, surface->width, surface->height,
              surface->memory, surface->image);
    REXLOG_INFO("[render] new surface {:08X}, {}x{}{}", address, surface->width, surface->height, depth ? ", depth" : "");
    Remember(fmt::format("surface {:08X}, {}x{}", address, surface->width, surface->height));
    surfaces_.push_back(std::move(surface));
    return surfaces_.back().get();
  }

  void BindBlend(uint32_t blend, uint32_t alpha_control) {
    const uint32_t colour_source = blend & 31, colour_dest = (blend >> 8) & 31;
    const uint32_t alpha_source = (blend >> 16) & 31, alpha_dest = (blend >> 24) & 31;
    const uint32_t colour_op = (blend >> 5) & 7, alpha_op = (blend >> 21) & 7;
    static constexpr DkBlendOp kBlendOp[8] = {DkBlendOp_Add, DkBlendOp_Sub, DkBlendOp_Min, DkBlendOp_Max,
                                              DkBlendOp_RevSub, DkBlendOp_Add, DkBlendOp_Add, DkBlendOp_Add};
    const bool blended = !(colour_source == 1 && colour_dest == 0 && alpha_source == 1 && alpha_dest == 0 &&
                           colour_op == 0 && alpha_op == 0);
    commands_.bindColorState(dk::ColorState{}.setBlendEnable(0, blended).setAlphaCompareOp(
        (alpha_control & 8) ? CompareOp(alpha_control) : DkCompareOp_Always));
    if (blended) {
      commands_.bindBlendStates(0, {dk::BlendState{}
                                        .setOps(kBlendOp[colour_op], kBlendOp[alpha_op])
                                        .setFactors(BlendFactor(colour_source), BlendFactor(colour_dest),
                                                    BlendFactor(alpha_source), BlendFactor(alpha_dest))});
    }
  }

  // RB_DEPTHCONTROL and RB_STENCILREFMASK. Stencil was checked against captured frames in tools/capsim.py.
  // Bit 7 (BACKFACE_ENABLE) gives back faces their own function and ops (bits 20-31) and reference and masks
  // (RB_STENCILREFMASK_BF); without it both faces use the front set.
  void BindDepthStencil(uint32_t depth_control, uint32_t stencil_word, uint32_t stencil_back = 0) {
    static constexpr DkStencilOp kStencilOp[8] = {DkStencilOp_Keep,   DkStencilOp_Zero,    DkStencilOp_Replace,
                                                  DkStencilOp_Incr,   DkStencilOp_Decr,    DkStencilOp_Invert,
                                                  DkStencilOp_IncrWrap, DkStencilOp_DecrWrap};
    const bool stencil_on = (depth_control & 1) != 0;
    const DkCompareOp compare = CompareOp(depth_control >> 8);
    const DkStencilOp pass = kStencilOp[(depth_control >> 14) & 7];
    const DkStencilOp fail = kStencilOp[(depth_control >> 11) & 7];
    const DkStencilOp depth_fail = kStencilOp[(depth_control >> 17) & 7];
    const bool two_sided = stencil_on && (depth_control & 0x80) != 0;
    const DkCompareOp back_compare = two_sided ? CompareOp(depth_control >> 20) : compare;
    const DkStencilOp back_fail = two_sided ? kStencilOp[(depth_control >> 23) & 7] : fail;
    const DkStencilOp back_pass = two_sided ? kStencilOp[(depth_control >> 26) & 7] : pass;
    const DkStencilOp back_depth_fail = two_sided ? kStencilOp[(depth_control >> 29) & 7] : depth_fail;
    commands_.bindDepthStencilState(dk::DepthStencilState{}
                                        .setDepthTestEnable((depth_control & 2) != 0)
                                        .setDepthWriteEnable((depth_control & 6) == 6)
                                        .setDepthCompareOp(CompareOp(depth_control >> 4))
                                        .setStencilTestEnable(stencil_on)
                                        .setStencilFrontCompareOp(compare)
                                        .setStencilFrontPassOp(pass)
                                        .setStencilFrontFailOp(fail)
                                        .setStencilFrontDepthFailOp(depth_fail)
                                        .setStencilBackCompareOp(back_compare)
                                        .setStencilBackPassOp(back_pass)
                                        .setStencilBackFailOp(back_fail)
                                        .setStencilBackDepthFailOp(back_depth_fail));
    if (two_sided) {
      commands_.setStencil(DkFace_Front, uint8_t(stencil_word >> 16), uint8_t(stencil_word),
                           uint8_t(stencil_word >> 8));
      commands_.setStencil(DkFace_Back, uint8_t(stencil_back >> 16), uint8_t(stencil_back),
                           uint8_t(stencil_back >> 8));
    } else if (stencil_on) {
      commands_.setStencil(DkFace_FrontAndBack, uint8_t(stencil_word >> 16), uint8_t(stencil_word),
                           uint8_t(stencil_word >> 8));
    }
  }

  // Copies a rectangle with a draw. mode 1 takes one channel, for depth into an R32F surface.
  void Blit(uint32_t source, const dk::ImageView& dest, uint32_t dest_width, uint32_t dest_height, uint32_t source_x,
            uint32_t source_y, uint32_t dest_x, uint32_t dest_y, uint32_t width, uint32_t height, int32_t mode,
            dk::Shader* fragment = nullptr) {
    Arena block = Allocate(256, DK_UNIFORM_BUF_ALIGNMENT);
    if (!block.cpu) return;
    const float data[8] = {float(dest_x) * 2.0f / float(dest_width) - 1.0f,
                           1.0f - float(dest_y) * 2.0f / float(dest_height),
                           float(dest_x + width) * 2.0f / float(dest_width) - 1.0f,
                           1.0f - float(dest_y + height) * 2.0f / float(dest_height),
                           float(source_x),
                           float(source_y),
                           float(source_x + width),
                           float(source_y + height)};
    const int32_t modes[4] = {mode, 0, 0, 0};
    std::memcpy(block.cpu, data, sizeof(data));
    std::memcpy(block.cpu + sizeof(data), modes, sizeof(modes));
    commands_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    commands_.bindRenderTargets(&dest);
    commands_.setViewports(0, {DkViewport{0.0f, 0.0f, float(dest_width), float(dest_height), 0.0f, 1.0f}});
    commands_.setScissors(0, {DkScissor{dest_x, dest_y, width, height}});
    commands_.bindShaders(DkStageFlag_GraphicsMask, {&blit_vertex_, fragment ? fragment : &blit_fragment_});
    commands_.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
    commands_.bindVtxAttribState({});
    commands_.bindVtxBufferState({});
    commands_.bindColorState(dk::ColorState{});
    commands_.bindColorWriteState(dk::ColorWriteState{}.setMask(0, DkColorMask_RGBA));
    commands_.bindDepthStencilState(dk::DepthStencilState{}.setDepthTestEnable(false).setDepthWriteEnable(false));
    commands_.bindUniformBuffer(DkStage_Vertex, 0, block.gpu, 256);
    commands_.bindUniformBuffer(DkStage_Fragment, 0, block.gpu, 256);
    commands_.bindTextures(DkStage_Fragment, 0, {dkMakeTextureHandle(source, 0)});
    pixel_bound_[0] = ~0u;
    commands_.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);
    commands_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    current_colour_ = current_depth_ = nullptr;
    program_ = Program::kNone;
  }

  static bool ImageFormat(uint32_t format, DkImageFormat* out) {
    switch (format) {
      case 2: *out = DkImageFormat_R8_Unorm; return true;
      case 3: *out = DkImageFormat_RGBA8_Unorm; return true;  // expanded in Upload
      case 6: *out = DkImageFormat_RGBA8_Unorm; return true;
      case 10: *out = DkImageFormat_RG8_Unorm; return true;
      case 18: *out = DkImageFormat_RGBA_BC1; return true;
      case 19: *out = DkImageFormat_RGBA_BC2; return true;
      case 20: *out = DkImageFormat_RGBA_BC3; return true;
      default: return false;
    }
  }

  bool CreateTexture(const TextureRequest& r) {
    DkImageFormat format;
    if (!ImageFormat(r.format, &format)) return false;
    // Formats decode these in hardware, with filtering done on the decoded values as on the console.
    if (GammaFetch(r.signs) && !r.full_hash) {
      if (r.format == 6) format = DkImageFormat_RGBA8_Unorm_sRGB;
      if (r.format == 18) format = DkImageFormat_RGBA_BC1_sRGB;
      if (r.format == 19) format = DkImageFormat_RGBA_BC2_sRGB;
      if (r.format == 20) format = DkImageFormat_RGBA_BC3_sRGB;
    } else if (SignedFetch(r.signs) && r.format == 6) {
      format = DkImageFormat_RGBA8_Snorm;
    } else if (SignedFetch(r.signs) && r.format == 10) {
      // Road stripes (sims3_098 screens): the roads' 8:8 normal map is fetched signed. Read as unsigned, every slightly
      // negative texel (the tyre-track grooves) came out near +1, a steep slope reflecting the sky in cyan dashes.
      format = DkImageFormat_RG8_Snorm;
    } else if (SignedFetch(r.signs) && r.format == 2) {
      format = DkImageFormat_R8_Snorm;
    }
    bool compressed = r.format >= 18 && r.format <= 20;
    const uint32_t width = compressed ? (r.width + 3) & ~3u : r.width;
    const uint32_t height = compressed ? (r.height + 3) & ~3u : r.height;
    // Half icons: small compressed textures (button icons, moodlets) sampled with their right half wrong even when
    // the copy engine reads them back whole (sims3_080). Kept as RGBA8, decoded on the CPU, they cannot disagree.
    const bool decode = kDecodeSmallBc && compressed && !r.cube && height <= 64 && width <= 256;
    if (decode) {
      const bool srgb = format == DkImageFormat_RGBA_BC1_sRGB || format == DkImageFormat_RGBA_BC2_sRGB ||
                        format == DkImageFormat_RGBA_BC3_sRGB;
      format = srgb ? DkImageFormat_RGBA8_Unorm_sRGB : DkImageFormat_RGBA8_Unorm;
      compressed = false;
    }
    dk::ImageLayout layout;
    dk::ImageLayoutMaker maker{device_};
    maker.setFormat(format);
    const uint32_t mip_levels = decode || r.cube ? 1 : r.mip_levels;
    if (mip_levels > 1) maker.setMipLevels(mip_levels);
    if (r.cube) {
      maker.setType(DkImageType_Cubemap).setDimensions(width, height, 6);
    } else {
      maker.setDimensions(width, height);
    }
    maker.initialize(layout);
    const uint64_t align = layout.getAlignment();
    const uint64_t start = (texture_used_ + align - 1) & ~(align - 1);
    // Half icons (sims3_077-078): a 32x32 DXT5 icon drew only its left half. Read back at draw time, its right half
    // (its second 512-byte GOB column) held another texture's blocks, though deko3d sizes it at 1024 bytes and the
    // next image starts there. For compressed formats the GOB columns are laid out as tall as the pixel height asks
    // (32 rows: 4 GOBs), not the block rows (8: 1 GOB), so the right column sits past the reported size. Each image
    // gets that footprint: GOB columns of 64 bytes, each as tall as a block of up to 16 GOBs picked from the pixel
    // height, rounded up to 512 bytes.
    const uint64_t element = r.format == 18 ? 8 : compressed ? 16 : 4;
    const uint64_t across = compressed ? width / 4 : width, down = compressed ? height / 4 : height;
    uint64_t block_gobs = 1;
    while (block_gobs < 16 && block_gobs * 8 < height) block_gobs *= 2;
    const uint64_t gob_rows = (down + 7) / 8, tall_gobs = (gob_rows + block_gobs - 1) / block_gobs * block_gobs;
    const uint64_t estimate = ((across * element + 63) / 64) * tall_gobs * 512 * (r.cube ? 6 : 1);
    const uint64_t size = (std::max<uint64_t>(layout.getSize(), estimate) + 511) & ~uint64_t(511);
    if (compressed && size != layout.getSize() && layout_logs_ < 12) {
      ++layout_logs_;
      REXLOG_INFO("[ui] padded {}x{} format {}: size {} alignment {}, kept {}, at {:X}", width, height, r.format,
                  layout.getSize(), align, size, start);
    }
    if (start + size > kTexturePool || next_descriptor_ >= persistent_descriptor_) return false;
    Entry& entry = textures_[r.key];
    entry.decoded = decode;
    entry.mip_levels = mip_levels;
    entry.image.initialize(layout, texture_memory_, uint32_t(start));
    texture_used_ = start + size;
    entry.aligned_width = width;
    entry.aligned_height = height;
    entry.texture.id = next_descriptor_++;
    entry.texture.u_scale = float(r.width) / float(width);
    entry.texture.v_scale = float(r.height) / float(height);
    entry.texture.valid = true;
    Remember(fmt::format("texture format {}, {}x{}, tiled {}, cube {}, pitch {}", r.format, r.width, r.height, r.tiled,
                         r.cube, r.pitch));
    dk::ImageView view{entry.image};
    view.setSwizzle(ImageSwizzle(r.swizzle[0]), ImageSwizzle(r.swizzle[1]), ImageSwizzle(r.swizzle[2]),
                    ImageSwizzle(r.swizzle[3]));
    image_descriptors_[entry.texture.id].initialize(view);
    uploads_.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
    uploaded_ = true;
    return true;
  }

  // BC1/BC2/BC3 blocks (row by row, `columns` x `rows`) to RGBA8 texels, rows of columns * 4 texels.
  static void DecodeBlocks(const uint8_t* blocks, uint32_t format, uint32_t columns, uint32_t rows, uint8_t* out) {
    const uint32_t block_bytes = format == 18 ? 8 : 16, stride = columns * 16;
    for (uint32_t by = 0; by < rows; ++by) {
      for (uint32_t bx = 0; bx < columns; ++bx) {
        const uint8_t* b = blocks + (size_t(by) * columns + bx) * block_bytes;
        const uint8_t* colour = format == 18 ? b : b + 8;
        const uint16_t c0 = uint16_t(colour[0] | (colour[1] << 8)), c1 = uint16_t(colour[2] | (colour[3] << 8));
        const uint32_t bits = uint32_t(colour[4]) | (uint32_t(colour[5]) << 8) | (uint32_t(colour[6]) << 16) |
                              (uint32_t(colour[7]) << 24);
        uint8_t palette[4][4];
        const auto expand = [](uint16_t c, uint8_t* p) {
          p[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
          p[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
          p[2] = uint8_t((c & 31) * 255 / 31);
          p[3] = 255;
        };
        expand(c0, palette[0]);
        expand(c1, palette[1]);
        const bool three = format == 18 && c0 <= c1;
        for (int i = 0; i < 3; ++i) {
          if (three) {
            palette[2][i] = uint8_t((palette[0][i] + palette[1][i]) / 2);
            palette[3][i] = 0;
          } else {
            palette[2][i] = uint8_t((2 * palette[0][i] + palette[1][i]) / 3);
            palette[3][i] = uint8_t((palette[0][i] + 2 * palette[1][i]) / 3);
          }
        }
        palette[2][3] = 255;
        palette[3][3] = three ? 0 : 255;
        uint8_t alpha[8] = {255, 255, 255, 255, 255, 255, 255, 255};
        uint64_t alpha_bits = 0;
        if (format == 20) {
          const uint32_t a0 = b[0], a1 = b[1];
          alpha[0] = uint8_t(a0);
          alpha[1] = uint8_t(a1);
          if (a0 > a1) {
            for (uint32_t i = 1; i < 7; ++i) alpha[i + 1] = uint8_t((a0 * (7 - i) + a1 * i) / 7);
          } else {
            for (uint32_t i = 1; i < 5; ++i) alpha[i + 1] = uint8_t((a0 * (5 - i) + a1 * i) / 5);
            alpha[6] = 0;
            alpha[7] = 255;
          }
          for (int i = 0; i < 6; ++i) alpha_bits |= uint64_t(b[2 + i]) << (8 * i);
        }
        for (uint32_t i = 0; i < 16; ++i) {
          uint8_t* texel = out + size_t(by * 4 + i / 4) * stride + (bx * 4 + i % 4) * 4;
          std::memcpy(texel, palette[(bits >> (2 * i)) & 3], 4);
          if (format == 20) texel[3] = alpha[(alpha_bits >> (3 * i)) & 7];
          if (format == 19) texel[3] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
        }
      }
    }
  }

  bool Upload(const TextureRequest& r, Entry& entry) {
    if (!UploadLevel(r, entry, 0, entry.aligned_width, entry.aligned_height)) return false;
    if (entry.mip_levels <= 1 || !r.mip_guest) return true;
    GuestMip mips[16];
    const uint32_t levels = std::min<uint32_t>(entry.mip_levels, 16);
    GuestMips(r.format, r.width, r.height, levels, r.packed_mips, mips);
    for (uint32_t level = 1; level < levels; ++level) {
      const GuestMip& m = mips[level];
      if (uint64_t(m.offset) + m.bytes > r.mip_bytes) return false;
      TextureRequest l = r;
      l.guest = r.mip_guest + m.offset;
      l.guest_bytes = m.bytes;
      l.width = m.width;
      l.height = m.height;
      l.pitch = m.pitch;
      l.packed_x = m.packed_x;
      l.packed_y = m.packed_y;
      l.full_hash = false;
      if (!UploadLevel(l, entry, level, m.width, m.height)) return false;
    }
    return true;
  }

  bool UploadLevel(const TextureRequest& r, Entry& entry, uint32_t level, uint32_t rect_width, uint32_t rect_height) {
    const bool compressed = r.format >= 18 && r.format <= 20;
    const uint32_t block = compressed ? 4 : 1;
    const uint32_t log2 = r.format == 18 ? 3 : compressed ? 4 : r.format == 2 ? 0 : (r.format == 3 || r.format == 10) ? 1 : 2;
    const uint32_t bytes = 1u << log2;
    const uint32_t columns = (r.width + block - 1) / block, rows = (r.height + block - 1) / block;
    const uint32_t pitch = r.pitch / block;
    const uint32_t out_bytes = r.format == 3 ? 4 : bytes;
    const size_t total = size_t(columns) * rows * out_bytes;
    const uint32_t faces = r.cube ? 6 : 1;
    if (staging_used_ + total * faces * (entry.decoded ? 8 : 1) > staging_base_ + kStagingMemory) return false;
    scratch_.resize(total);
    for (uint32_t face = 0; face < faces; ++face) {
    const uint8_t* guest = r.guest + size_t(face) * r.slice_bytes;
    const uint32_t guest_bytes = r.cube ? r.slice_bytes : r.guest_bytes;
    uint8_t* out = scratch_.data();
    // Tiled with 4 bytes or more per element: an offset is its 32x32 tile's base plus an offset inside the tile
    // that depends only on (x & 31, y & 31) (TiledOffset's macro part is a multiple of 512 there), so a table
    // replaces the per-texel arithmetic. 8- and 16-bit formats mix the two and take the general loop.
    if (r.tiled && log2 >= 2 && r.format != 3) {
      uint32_t inside[32 * 32];
      for (uint32_t ty = 0; ty < 32; ++ty) {
        for (uint32_t tx = 0; tx < 32; ++tx) inside[ty * 32 + tx] = TiledOffset(tx, ty, pitch, log2);
      }
      const uint32_t tiles_across = ((pitch + 31) & ~31u) >> 5, tile_shift = log2 + 10;
      for (uint32_t y = 0; y < rows; ++y) {
        const uint32_t gy = y + r.packed_y / block;
        const uint32_t* row = inside + (gy & 31) * 32;
        const uint32_t row_tiles = (gy >> 5) * tiles_across;
        for (uint32_t x = 0; x < columns; ++x, out += bytes) {
          const uint32_t gx = x + r.packed_x / block;
          const uint32_t offset = (((gx >> 5) + row_tiles) << tile_shift) + row[gx & 31];
          if (uint64_t(offset) + bytes > guest_bytes) {
            std::memset(out, 0, bytes);
            continue;
          }
          const uint8_t* from = guest + offset;
          if (r.endian == 2) {
            for (uint32_t i = 0; i < bytes; i += 4) {
              uint32_t word;
              std::memcpy(&word, from + i, 4);
              word = std::byteswap(word);
              std::memcpy(out + i, &word, 4);
            }
          } else if (r.endian == 1) {
            for (uint32_t i = 0; i < bytes; i += 2) {
              out[i] = from[i + 1];
              out[i + 1] = from[i];
            }
          } else {
            std::memcpy(out, from, bytes);
          }
        }
      }
    } else if (!r.tiled && r.format != 3 && !r.packed_x && !r.packed_y &&
               uint64_t(rows - 1) * pitch * bytes + uint64_t(columns) * bytes <= guest_bytes) {
      // Linear and whole (sims3_087: CPU-written 8-bit maps re-uploaded every few frames took ~1.2 ms each in
      // the per-texel loop): a row at a time.
      const size_t row_bytes = size_t(columns) * bytes;
      for (uint32_t y = 0; y < rows; ++y, out += row_bytes) {
        const uint8_t* from = guest + size_t(y) * pitch * bytes;
        if (r.endian == 2 && bytes >= 4) {
          for (size_t i = 0; i + 4 <= row_bytes; i += 4) {
            uint32_t word;
            std::memcpy(&word, from + i, 4);
            word = std::byteswap(word);
            std::memcpy(out + i, &word, 4);
          }
        } else if (r.endian == 1 && bytes >= 2) {
          for (size_t i = 0; i + 2 <= row_bytes; i += 2) {
            out[i] = from[i + 1];
            out[i + 1] = from[i];
          }
        } else {  // as the texel loop: no swap for 8-bit texels, or for 16-bit ones under 8in32
          std::memcpy(out, from, row_bytes);
        }
      }
    } else
    for (uint32_t y = 0; y < rows; ++y) {
      for (uint32_t x = 0; x < columns; ++x, out += out_bytes) {
        const uint32_t gx = x + r.packed_x / block, gy = y + r.packed_y / block;
        const uint32_t offset = r.tiled ? TiledOffset(gx, gy, pitch, log2) : (gy * pitch + gx) * bytes;
        if (uint64_t(offset) + bytes > guest_bytes) {
          std::memset(out, 0, out_bytes);
          continue;
        }
        uint8_t texel[16];
        std::memcpy(texel, guest + offset, bytes);
        if (r.endian == 1) {
          for (uint32_t i = 0; i + 1 < bytes; i += 2) std::swap(texel[i], texel[i + 1]);
        } else if (r.endian == 2 && bytes >= 4) {
          for (uint32_t i = 0; i + 3 < bytes; i += 4) {
            std::swap(texel[i], texel[i + 3]);
            std::swap(texel[i + 1], texel[i + 2]);
          }
        }
        if (r.format == 3) {
          const uint32_t v = texel[0] | (uint32_t(texel[1]) << 8);
          out[0] = uint8_t(((v & 31) * 255 + 15) / 31);
          out[1] = uint8_t((((v >> 5) & 31) * 255 + 15) / 31);
          out[2] = uint8_t((((v >> 10) & 31) * 255 + 15) / 31);
          out[3] = (v >> 15) ? 255 : 0;
        } else {
          std::memcpy(out, texel, bytes);
        }
      }
    }
    // Black shade: the ambient cube map is CPU-built from a GPU probe. What each face holds when uploaded.
    if (r.cube && cube_logs_ < 60) {
      ++cube_logs_;
      double sum[4] = {};
      const size_t texels = total / 4;
      for (size_t i = 0; i < texels; ++i) {
        for (int c = 0; c < 4; ++c) sum[c] += scratch_[i * 4 + c];
      }
      const double n = texels ? double(texels) * 255.0 : 1.0;
      REXLOG_INFO("[render] cube upload {:08X} {}x{} face {} frame {}: mean {:.3f} {:.3f} {:.3f} {:.3f}",
                  (r.key[1] >> 12) << 12, r.width, r.height, face, frame_, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n);
    }
    // Button icons show only their left half (sims3_049-050: drawn whole, nothing opaque on top). The first
    // uploads of 32x32 DXT5 interface textures are saved as guest bytes then the converted blocks.
    // Also the Sim's 512x256 DXT5 composite used by headshots (green portraits), from any path.
    if (r.format == 20 && r.width == 512 && r.height == 256 && sim_atlas_logs_ < 200) {
      ++sim_atlas_logs_;
      REXLOG_INFO("[render] sim atlas reupload {:08X} frame {}", (r.key[1] >> 12) << 12, frame_);
    }
    const bool sim_atlas = r.format == 20 && r.width == 512 && r.height == 256 && sim_atlas_dumps_ < 8;
    if (sim_atlas && !icon_root_.empty()) {
      const std::string path = fmt::format("{}/simatlas_{:08X}_{}.bin", icon_root_, (r.key[1] >> 12) << 12, sim_atlas_dumps_++);
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(r.guest, 1, r.guest_bytes, f);
        std::fwrite(scratch_.data(), 1, total, f);
        std::fclose(f);
      }
      REXLOG_INFO("[render] sim atlas upload {:08X} frame {}: endian {}, signs {:02X}, swizzle {}{}{}{}, words {:08X} {:08X} {:08X} {:08X}",
                  (r.key[1] >> 12) << 12, frame_, r.endian, r.signs, r.swizzle[0], r.swizzle[1], r.swizzle[2], r.swizzle[3],
                  r.key[0], r.key[1], r.key[2], r.key[3]);
    }
    // The 4x4 1555 texture the composite passes paint with (a skin or tint colour?): its raw words and texels.
    if (r.format == 3 && r.width <= 4 && r.height <= 4 && small_logs_ < 40 && r.guest_bytes >= 8 && total >= 16) {
      ++small_logs_;
      const uint8_t* g = r.guest;
      REXLOG_INFO("[render] 1555 upload {:08X} {}x{}: endian {}, swizzle {}{}{}{}, guest {:02X}{:02X} {:02X}{:02X} "
                  "{:02X}{:02X} {:02X}{:02X}, texel0 {} {} {} {}, texel1 {} {} {} {}",
                  (r.key[1] >> 12) << 12, r.width, r.height, r.endian, r.swizzle[0], r.swizzle[1], r.swizzle[2],
                  r.swizzle[3], g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], scratch_[0], scratch_[1], scratch_[2],
                  scratch_[3], scratch_[4], scratch_[5], scratch_[6], scratch_[7]);
    }
    const bool atlas_dump = r.width == 1024 && r.height == 256 && atlas_dumps_ < 12;
    if (r.full_hash && r.format == 20 && ((r.width == 32 && r.height == 32 && icon_dumps_ < 24) || atlas_dump) &&
        !icon_root_.empty()) {
      if (atlas_dump) ++atlas_dumps_;
      const uint32_t address = (r.key[1] >> 12) << 12;
      const std::string path = fmt::format("{}/icon_{:08X}_{}.bin", icon_root_, address, icon_dumps_++);
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(r.guest, 1, r.guest_bytes, f);
        std::fwrite(scratch_.data(), 1, total, f);
        std::fclose(f);
      }
      REXLOG_INFO("[ui] icon upload {:08X} frame {}: pitch {}, tiled {}, endian {}, packed {} {}, guest {} bytes, "
                  "{} converted, words {:08X} {:08X} {:08X} {:08X}",
                  address, frame_, r.pitch, r.tiled, r.endian, r.packed_x, r.packed_y, r.guest_bytes, total, r.key[0],
                  r.key[1], r.key[2], r.key[3]);
    }
    auto* staging = static_cast<uint8_t*>(staging_memory_.getCpuAddr()) + staging_used_;
    size_t staged = total;
    if (entry.decoded) {
      DecodeBlocks(scratch_.data(), r.format, columns, rows, staging);
      staged = size_t(columns) * rows * 64;
    } else {
      std::memcpy(staging, scratch_.data(), total);
    }
    dk::ImageView view{entry.image};
    view.setMipLevels(uint8_t(level), 1);
    uploads_.copyBufferToImage(DkCopyBuf{staging_memory_.getGpuAddr() + staging_used_, 0, 0}, view,
                               DkImageRect{0, 0, face, rect_width, rect_height, 1});
    staging_used_ += (staged + 0xFF) & ~size_t(0xFF);
    }
    uploaded_ = true;
    // Half icons (sims3_066: drawn whole, nothing on top, uploaded data whole): the GPU's own copy, read back.
    if (r.full_hash && r.format == 20 && r.width == 32 && r.height == 32 && false && !icon_root_.empty()) {
      const std::string path =
          fmt::format("{}/gpuicon_{:08X}_{}.bin", icon_root_, (r.key[1] >> 12) << 12, gpu_icon_dumps_++);
      FlushFrame();
      dk::ImageView view{entry.image};
      commands_.copyImageToBuffer(view, DkImageRect{0, 0, 0, 32, 32, 1}, DkCopyBuf{staging_memory_.getGpuAddr(), 0, 0});
      queue_.submitCommands(commands_.finishList());
      queue_.waitIdle();
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(staging_memory_.getCpuAddr(), 1, 1024, f);
        std::fclose(f);
      }
      current_colour_ = current_depth_ = nullptr;
      program_ = Program::kNone;
      REXLOG_INFO("[ui] read back {} (image {}x{})", path, entry.aligned_width, entry.aligned_height);
    }
    return true;
  }
  uint32_t gpu_icon_dumps_ = 0;
  uint32_t layout_logs_ = 0;

 public:
  // Half icons, test runs: the GPU's copy of a 32x32 compressed texture, by descriptor, read back now (1024 bytes).
  bool ReadIcon(uint32_t id, std::vector<uint8_t>* out) {
    for (auto& [key, entry] : textures_) {
      if (entry.texture.id != id || entry.aligned_width != 32 || entry.aligned_height != 32) continue;
      FlushFrame();
      dk::ImageView view{entry.image};
      commands_.copyImageToBuffer(view, DkImageRect{0, 0, 0, 32, 32, 1}, DkCopyBuf{staging_memory_.getGpuAddr(), 0, 0});
      queue_.submitCommands(commands_.finishList());
      queue_.waitIdle();
      const auto* data = static_cast<const uint8_t*>(staging_memory_.getCpuAddr());
      out->assign(data, data + 1024);
      // Then the raw memory from 1 KB before the image to 8 KB after, its descriptor (32 bytes) and the image
      // object, to see where the copy engine put the right half and what the sampler is told.
      const DkGpuAddr base = entry.image.getGpuAddr();
      commands_.copyBuffer(base - 1024, staging_memory_.getGpuAddr(), 9216);
      queue_.submitCommands(commands_.finishList());
      queue_.waitIdle();
      out->insert(out->end(), data, data + 9216);
      const auto* descriptor = reinterpret_cast<const uint8_t*>(&image_descriptors_[id]);
      out->insert(out->end(), descriptor, descriptor + 32);
      const auto* object = reinterpret_cast<const uint8_t*>(&entry.image);
      out->insert(out->end(), object, object + sizeof(entry.image));
      REXLOG_INFO("[probe] icon image at {:X} (pool {:X}), image object {} bytes", base, texture_memory_.getGpuAddr(),
                  sizeof(entry.image));
      current_colour_ = current_depth_ = nullptr;
      program_ = Program::kNone;
      return true;
    }
    return false;
  }

 private:

  dk::UniqueDevice device_;
  dk::UniqueMemBlock image_memory_;
  dk::Image images_[kFramebuffers];
  dk::UniqueSwapchain swapchain_;
  dk::UniqueQueue queue_;
  uint32_t queue_errors_ = 0, atlas_events_ = 0, overwrites_logged_ = 0;
  uint32_t raster_state_ = 0;  // bias enable, then the cull bits; 0 is what the other passes bind
  float bias_constant_ = 0, bias_slope_ = 0;
  std::unordered_set<uint64_t> refused_fetches_;
  std::deque<std::pair<uint32_t, std::string>> gone_;
  std::function<const uint8_t*(uint32_t, uint32_t)> guest_;
  std::array<uint32_t, 32> pixel_bound_{};
  bool shaders_fresh_ = true, wrote_back_ = false;
  std::string step_root_;
  uint32_t steps_saved_ = 0;
  std::string portrait_root_;
  uint32_t portrait_armed_ = 0, portraits_saved_ = 0;
  uint64_t portrait_frame_ = ~0ull;
  uint32_t write_epoch_ = 0;
  struct ResolveRun {
    uint64_t frame = 0;
    uint32_t count = 0;  // consecutive frames with a resolve here
  };
  std::unordered_map<uint32_t, ResolveRun> resolve_last_;
  std::bitset<kDescriptorSlots> used_descriptors_;
  std::unordered_map<uint64_t, uint64_t> pair_frame_;
  std::string rare_draws_;
  std::vector<uint32_t> free_descriptors_;
  uint32_t blend_words_[4] = {};
  bool blend_valid_ = false;
  uint32_t command_extra_used_ = 0, command_extra_peak_ = 0, frame_draws_ = 0;
  size_t flushed_used_ = 0;
  uint32_t chunk_draws_ = 0, chunks_ = 0, flushes_ = 0;
  std::deque<std::string> recent_;
  dk::Shader blit_vertex_, blit_fragment_, blit_depth_fragment_, blit_stencil_fragment_, stencil_bit_fragment_;
  dk::UniqueMemBlock xenos_memory_, frame_memory_, cube_memory_, safe_memory_, vertex_cache_memory_;
  size_t vertex_cache_used_ = 0;
  uint32_t vertex_cache_generation_ = 1;  // entries start at 0, so none is valid before its first fill
  bool vertex_cache_full_ = false;
  dk::Image cube_image_, safe_image_;
  uint32_t xenos_used_ = 0, cube_descriptor_ = 0;
  uint32_t persistent_descriptor_ = kDescriptorSlots;
  size_t frame_used_ = 0;
  std::unordered_map<uint64_t, LoadedShader> shaders_;
  std::vector<std::unique_ptr<Target>> colours_, depths_;
  std::vector<std::unique_ptr<Surface>> surfaces_, retired_;
  std::vector<std::unique_ptr<Surface>> slot_retired_[kFrameSlots];  // freed once the slot's frame is done
  Target* current_colour_ = nullptr;
  Target* current_depth_ = nullptr;
  Program program_ = Program::kNone;
  dk::UniqueMemBlock command_memory_;
  dk::UniqueCmdBuf commands_;
  dk::UniqueCmdBuf uploads_;
  dk::UniqueMemBlock data_memory_;
  dk::UniqueMemBlock texture_memory_;
  dk::UniqueMemBlock staging_memory_;
  dk::UniqueMemBlock shader_memory_;
  dk::Shader vertex_shader_;
  dk::Shader fragment_shader_;
  UiVertex* vertices_ = nullptr;
  dk::ImageDescriptor* image_descriptors_ = nullptr;
  dk::SamplerDescriptor* sampler_descriptor_ = nullptr;
  DkGpuAddr uniform_address_ = 0;
  std::map<std::array<uint32_t, 4>, Entry> textures_;
  std::vector<uint8_t> scratch_;
  uint32_t vertex_used_ = 0, next_descriptor_ = 0;
  uint64_t texture_used_ = 0, frame_ = 0;
  size_t staging_used_ = 0, staging_base_ = 0, frame_base_ = 0;
  uint32_t slot_index_ = 0;
  dk::Fence frame_fences_[kFrameSlots];
  bool fence_pending_[kFrameSlots] = {};
  dk::UniqueMemBlock ui_vertex_memory_;
  dk::UniqueMemBlock readback_memory_;
  int slot_ = 0;
  bool open_ = false, uploaded_ = false;
};

class Deko3dGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  Deko3dGraphicsSystem() : registers_(kRegisterCount, 0) {}
  ~Deko3dGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext*) override {
    if (!presenting_) presenting_ = presenter_.Create();
    return presenting_ ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!mmio_registered_) {
      mmio_registered_ =
          memory_->AddVirtualMappedRange(kMmioBase, kMmioMask, kMmioSize, this, &MmioRead, &MmioWrite);
      if (!mmio_registered_) {
        REXLOG_ERROR("[sims3-gpu] could not register the GPU MMIO range");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (active_.exchange(true)) return X_STATUS_SUCCESS;
    vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return VblankLoop(); }));
    vblank_thread_->set_name("GPU VSync");
    vblank_thread_->Create();
    ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 256 * 1024, 0, [this]() { return RingLoop(); }));
    ring_thread_->set_name("GPU ring");
    ring_thread_->Create();
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenting_; }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[sims3-gpu] interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    ring_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    ring_base_.store(ptr, std::memory_order_release);
    ring_generation_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[sims3-gpu] ring at {:08X}, {} words", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t) override {
    read_writeback_.store(ptr, std::memory_order_release);
  }

  void Shutdown() override {
    if (active_.exchange(false)) {
      ring_cv_.notify_all();
      if (ring_thread_) {
        ring_thread_->Wait(0, 0, 0, nullptr);
        ring_thread_.reset();
      }
      if (vblank_thread_) {
        vblank_thread_->Wait(0, 0, 0, nullptr);
        vblank_thread_.reset();
      }
    }
    if (presenting_) {
      presenter_.Destroy();
      presenting_ = false;
    }
  }

 private:
  static uint32_t MmioRead(void*, void* ctx, uint32_t address) {
    return static_cast<Deko3dGraphicsSystem*>(ctx)->ReadMmio(address);
  }
  static void MmioWrite(void*, void* ctx, uint32_t address, uint32_t value) {
    static_cast<Deko3dGraphicsSystem*>(ctx)->WriteMmio(address, value);
  }

  uint32_t ReadMmio(uint32_t address) {
    const uint32_t r = (address & 0xFFFF) / 4;
    switch (r) {
      case kRegRbEdramTiming: return 0x08100748;
      case kRegRbBcControl: return 0x0000200E;
      case kRegD1ModeVCounter:
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        const uint32_t h = std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
        if (r == kRegD1ModeVCounter) return h;
        return (std::min(uint32_t(mode.display_width), uint32_t(0x0FFF)) << 16) | h;
      }
      case kRegD1ModeVblankVlineStatus: return 1;
      default: return r < kRegisterCount ? registers_[r] : 0;
    }
  }

  void WriteMmio(uint32_t address, uint32_t value) {
    const uint32_t r = (address & 0xFFFF) / 4;
    if (r == kRegCpRbWptr) {
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        write_pointer_.store(value, std::memory_order_release);
      }
      ring_cv_.notify_all();
      return;
    }
    WriteRegister(r, value);
  }

  void WriteRegister(uint32_t r, uint32_t value) {
    if (r >= kRegisterCount) return;
    registers_[r] = value;
    if (r >= 0x4000 && r < 0x4800) {
      const uint32_t vec = ((r - 0x4000) & 1023) >> 2;
      constant_bank_[(r - 0x4000) >> 10].dirty[vec >> 6] |= 1ull << (vec & 63);
    }
    if (r >= kRegFetchFirst && r <= kRegFetchLast && (r - kRegFetchFirst) % 6 == 5) {
      NoteFetchConstant(r - 5);
    }
    if (r >= kRegScratch0 && r <= kRegScratch7) {
      const uint32_t i = r - kRegScratch0;
      if (registers_[kRegScratchUmsk] & (1u << i)) {
        // Always big-endian, whatever the address bits say. Direct3D passes interrupt callbacks to
        // the guest handler through these words, so a little-endian store becomes a bad call target.
        const uint32_t swapped = std::byteswap(value);
        std::memcpy(memory_->TranslatePhysical((registers_[kRegScratchAddr] + i * 4) & 0x1FFFFFFC),
                    &swapped, 4);
      }
    }
  }

  uint32_t Register(uint32_t r) const { return r < kRegisterCount ? registers_[r] : 0; }

  uint32_t ReadMemory(uint32_t address) {
    uint32_t value;
    std::memcpy(&value, memory_->TranslatePhysical(address & 0x1FFFFFFC), 4);
    return GpuSwap(value, address & 3);
  }

  void WriteMemory(uint32_t address, uint32_t value) {
    value = GpuSwap(value, address & 3);
    std::memcpy(memory_->TranslatePhysical(address & 0x1FFFFFFC), &value, 4);
  }

  void Interrupt(uint32_t source, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback) return;
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) return;
    thread->SetActiveCpu(cpu);
    uint64_t args[] = {source, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
  }

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    while (active_.load(std::memory_order_acquire)) {
      const auto now = Clock::now();
      if (now - next > std::chrono::milliseconds(250)) next = now;
      while (now >= next) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  }

  // One packet source: the ring (wraps) or an indirect buffer (does not).
  struct Reader {
    const uint32_t* base = nullptr;
    uint32_t mask = 0xFFFFFFFF;
    uint32_t position = 0;
    uint32_t end = 0;
    bool Pending() const { return position != end; }
    uint32_t Read() {
      const uint32_t value = std::byteswap(base[position & mask]);
      position = mask == 0xFFFFFFFF ? position + 1 : (position + 1) & mask;
      return value;
    }
    void Skip(uint32_t words) {
      position = mask == 0xFFFFFFFF ? position + words : (position + words) & mask;
    }
  };

  int RingLoop() {
    uint32_t read = 0;
    uint32_t generation = ring_generation_.load(std::memory_order_acquire);
    while (active_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_cv_.wait_for(lock, std::chrono::milliseconds(5), [&] {
          const uint32_t words = ring_words_.load(std::memory_order_acquire);
          return !active_.load(std::memory_order_acquire) ||
                 (words && (write_pointer_.load(std::memory_order_acquire) & (words - 1)) != read);
        });
      }
      if (!active_.load(std::memory_order_acquire)) break;
      const uint32_t gen = ring_generation_.load(std::memory_order_acquire);
      if (gen != generation) {
        generation = gen;
        read = 0;
      }
      const uint32_t words = ring_words_.load(std::memory_order_acquire);
      const uint32_t base = ring_base_.load(std::memory_order_acquire);
      if (!words || !base) continue;
      const uint32_t write = write_pointer_.load(std::memory_order_acquire) & (words - 1);
      Report();
      if (write == read) continue;
      Reader reader;
      reader.base = reinterpret_cast<const uint32_t*>(memory_->TranslatePhysical(base & 0x1FFFFFFF));
      reader.mask = words - 1;
      reader.position = read;
      reader.end = write;
      census_.ring_words += (write - read) & (words - 1);
      const auto busy_start = Clock::now();
      const uint64_t waited_before = census_.wait_ns + census_.present_ns;
      {
        ProfileScope scope(kProfPacket);
        while (reader.Pending() && active_.load(std::memory_order_acquire)) Packet(reader, 0);
      }
      const uint64_t waited = census_.wait_ns + census_.present_ns - waited_before;
      const uint64_t spent = uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - busy_start).count());
      census_.busy_ns += spent > waited ? spent - waited : 0;
      read = reader.position;
      if (const uint32_t writeback = read_writeback_.load(std::memory_order_acquire)) {
        const uint32_t value = std::byteswap(read);
        std::memcpy(memory_->TranslatePhysical(writeback & 0x1FFFFFFC), &value, 4);
      }
    }
    return 0;
  }

  void Packet(Reader& data, int depth) {
    const uint32_t header = data.Read();
    switch (header >> 30) {
      case 0: {
        const uint32_t base = header & 0x7FFF;
        const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        const bool one_register = (header >> 15) & 1;
        for (uint32_t i = 0; i < count; ++i) WriteRegister(one_register ? base : base + i, data.Read());
        break;
      }
      case 1:
        WriteRegister(header & 0x7FF, data.Read());
        WriteRegister((header >> 11) & 0x7FF, data.Read());
        break;
      case 2:
        break;
      default:
        Type3(data, header, depth);
        break;
    }
  }

  void Type3(Reader& data, uint32_t header, int depth) {
    const uint32_t opcode = (header >> 8) & 0x7F;
    const uint32_t words = ((header >> 16) & 0x3FFF) + 1;
    uint32_t used = 0;
    auto next = [&]() {
      ++used;
      return data.Read();
    };
    ++census_.opcode[opcode];
    // The predicate bit covers every type 3 packet, not only draws (sdk/src/graphics/command_processor.cpp).
    // Direct3D marks its CPU callback waits and interrupts this way while it is tiling.
    if ((header & 1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      data.Skip(words);
      return;
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        const uint32_t cpus = next();
        ++census_.interrupts;
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) Interrupt(1, cpu);
        }
        break;
      }
      case xenos::PM4_XE_SWAP: {
        ++swaps_;
        ++census_.swaps;
        ++census_.total_swaps;
        if (words >= 4) {
          next();
          const uint32_t front = next(), width = next(), height = next();
          if (front != census_.frontbuffer || width != census_.front_width ||
              height != census_.front_height) {
            census_.frontbuffer = front;
            census_.front_width = width;
            census_.front_height = height;
            REXLOG_INFO("[census] frontbuffer {:08X}, {}x{}", front, width, height);
          }
        }
        counter_.fetch_add(1, std::memory_order_relaxed);
        const auto present_start = Clock::now();
        Present();
        census_.present_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           Clock::now() - present_start).count());
        EndDrawLogFrame();
        CaptureFrameEnd();
        break;
      }
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        if (opcode == xenos::PM4_DRAW_INDX) next();  // visibility query token
        const uint32_t initiator = next();
        const uint32_t dma_base = used < words ? next() : 0;
        const uint32_t dma_size = used < words ? next() : 0;
        NoteDraw(initiator, (header & 1) != 0);
        ++capture_.frame_draws;
        if (capture_.active) CaptureDraw(initiator, (header & 1) != 0, dma_base, dma_size);
        skip_ = DrawSkip::kDrawn;
        {
          ProfileScope scope(kProfDraw);
          RenderDraw(initiator, (header & 1) != 0, dma_base, dma_size);
          NoteRecentDraw(initiator);
        }
        NoteDrawInLog(initiator, (header & 1) != 0);
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | next();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(next()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | next();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(next()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK: {
        const uint64_t high = next(), low = next();
        bin_mask_ = (high << 32) | low;
        break;
      }
      case xenos::PM4_SET_BIN_SELECT: {
        const uint64_t high = next(), low = next();
        bin_select_ = (high << 32) | low;
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        const uint32_t type_index = next();
        const uint32_t constant_base = ConstantBase((type_index >> 16) & 0xFF);
        if (constant_base != UINT32_MAX) {
          uint32_t r = constant_base + (type_index & 0x7FF);
          while (used < words) WriteRegister(r++, next());
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        uint32_t r = next() & 0xFFFF;
        while (used < words) WriteRegister(r++, next());
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        const uint32_t address = next() & 0x1FFFFFFC;
        const uint32_t type_index = next();
        const uint32_t count = next() & 0xFFF;
        const uint32_t constant_base = ConstantBase((type_index >> 16) & 0xFF);
        if (constant_base != UINT32_MAX) {
          const uint8_t* source = memory_->TranslatePhysical(address);
          const uint32_t first = constant_base + (type_index & 0x7FF);
          for (uint32_t i = 0; i < count; ++i) {
            uint32_t value;
            std::memcpy(&value, source + i * 4, 4);
            WriteRegister(first + i, std::byteswap(value));
          }
        }
        break;
      }
      case xenos::PM4_IM_LOAD: {
        const uint32_t address_type = next();
        const uint32_t size = next() & 0xFFFF;
        NoteShader(address_type & 3, memory_->TranslatePhysical(address_type & 0x1FFFFFFC), size);
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {
        const uint32_t type = next();
        const uint32_t size = next() & 0xFFFF;
        std::vector<uint8_t> code(size_t(size) * 4);
        for (uint32_t i = 0; i < size && used < words; ++i) {
          // Back to guest byte order, so this hashes and saves like an IM_LOAD from memory.
          const uint32_t value = std::byteswap(next());
          std::memcpy(&code[size_t(i) * 4], &value, 4);
        }
        NoteShader(type & 3, code.data(), size);
        break;
      }
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        const uint32_t address = next() & 0x1FFFFFFF;
        const uint32_t length = next() & 0xFFFFF;
        ++census_.indirect_buffers;
        census_.ring_words += length;
        if (depth < kMaxIndirectDepth) {
          Reader buffer;
          buffer.base = reinterpret_cast<const uint32_t*>(memory_->TranslatePhysical(address));
          buffer.end = length;
          while (buffer.Pending() && buffer.position < length) Packet(buffer, depth + 1);
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        const uint32_t info = next(), poll = next(), ref = next(), mask = next();
        const auto wait_start = Clock::now();
        const auto limit = wait_start + kWaitMax;
        auto& site = census_.wait_sites[{poll, ref}];
        ++site.count;
        ++census_.waits;
        struct WaitTimer {
          Census& census;
          Census::WaitSite& site;
          Clock::time_point start;
          ~WaitTimer() {
            const uint64_t ns = uint64_t(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
            census.wait_ns += ns;
            site.ns += ns;
          }
        } wait_timer{census_, site, wait_start};
        for (;;) {
          if (!(info & 0x10) && poll == kRegCoherStatusHost && (registers_[poll] & 0x80000000u)) {
            registers_[poll] = 0;  // nothing to make coherent: no draws are running
          }
          const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
          if (Compare(info, value & mask, ref)) break;
          if (Clock::now() >= limit || !active_.load(std::memory_order_acquire)) {
            ++site.timeouts;
            ++census_.wait_timeouts;
            if (waits_timed_out_++ == 0) {
              REXLOG_WARN("[sims3-gpu] WAIT_REG_MEM not met ({} {:08X} ref {:08X} mask {:08X})",
                          (info & 0x10) ? "memory" : "register", poll, ref, mask);
            }
            break;
          }
          rex::thread::Sleep(std::chrono::microseconds(50));
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        const uint32_t info = next(), and_value = next(), or_value = next();
        uint32_t v = Register(info & 0x1FFF);
        v &= ((info >> 31) & 1) ? Register(and_value & 0x1FFF) : and_value;
        v |= ((info >> 30) & 1) ? Register(or_value & 0x1FFF) : or_value;
        WriteRegister(info & 0x1FFF, v);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        const uint32_t reg = next(), address = next();
        WriteMemory(address, Register(reg));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        uint32_t address = next();
        while (used < words) {
          WriteMemory(address, next());
          address += 4;
        }
        break;
      }
      case xenos::PM4_COND_WRITE: {
        const uint32_t info = next(), poll = next(), ref = next(), mask = next();
        const uint32_t dest = next(), value = next();
        const uint32_t v = (info & 0x10) ? ReadMemory(poll) : Register(poll);
        if (Compare(info, v & mask, ref)) {
          if (info & 0x100) {
            WriteMemory(dest, value);
          } else {
            WriteRegister(dest, value);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_SHD: {
        const uint32_t initiator = next(), address = next(), value = next();
        WriteMemory(address, ((initiator >> 31) & 1) ? counter_.load(std::memory_order_relaxed) : value);
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        next();
        const uint32_t address = next();
        // Direct3D's tiling extents: report the whole surface.
        const uint16_t extents[6] = {0, 2048 >> 3, 0, 2048 >> 3, 0, 1};
        uint8_t* out = memory_->TranslatePhysical(address & 0x1FFFFFFC);
        for (uint32_t i = 0; i < 6; ++i) {
          const uint16_t v = std::byteswap(extents[i]);
          std::memcpy(out + i * 2, &v, 2);
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        next();
        const uint32_t address = Register(kRegRbSampleCountAddr);
        if (address) {
          // No draws, so answer occlusion queries as "visible" and let the game carry on.
          auto* counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
          std::memset(counts, 0, sizeof(*counts));
          counts->ZPass_A = 1000;
          counts->Total_A = 1000;
        }
        break;
      }
      default:
        break;
    }
    if (used < words) data.Skip(words - used);
  }

  void NoteDraw(uint32_t initiator, bool predicated) {
    const uint32_t mode = Register(kRegRbModeControl) & 7;
    census_.predicated += predicated;
    if (mode == kEdramModeCopy) {
      ++census_.resolves;
      ++census_.total_resolves;
      ++capture_.frame_resolves;
      return;
    }
    ++census_.draws;
    ++census_.total_draws;
    census_.indices += initiator >> 16;
    ++census_.primitive[initiator & 0x3F];
    ++census_.source[(initiator >> 6) & 3];
    const uint32_t surface = Register(kRegRbSurfaceInfo);
    const uint32_t colour = (Register(kRegRbColorInfo) >> 16) & 0xF;
    const uint32_t depth = (Register(kRegRbDepthInfo) >> 16) & 1;
    const uint32_t mask = Register(kRegRbColorMask);
    uint32_t targets = 0;
    for (uint32_t i = 0; i < 4; ++i) targets += ((mask >> (i * 4)) & 0xF) != 0;
    ++census_.targets[(surface & 0x3FFF) | (((surface >> 16) & 3) << 14) | (colour << 16) |
                      (depth << 20) | (targets << 21)];
  }

  void NoteFetchConstant(uint32_t first) {
    const uint32_t word0 = registers_[first], word1 = registers_[first + 1];
    if ((word0 & 3) != 2) return;  // only texture fetch constants
    ++census_.texture_format[word1 & 0x3F];
  }

  // HashWords names the shader (the build's translated table is keyed by it) but goes a byte at a time, and the
  // game loads a shader for nearly every draw. A fast hash finds the earlier result, and a compare proves the
  // microcode is the same.
  uint64_t ShaderHash(const uint8_t* code, uint32_t dwords) {
    const size_t bytes = size_t(dwords) * 4;
    // The game reloads the same few shaders from the same places: a pointer cache skips the hash, and the
    // compare still proves the code is unchanged.
    ShaderSlot& slot = shader_slots_[(reinterpret_cast<uintptr_t>(code) >> 4) & 255];
    if (slot.code == code && slot.bytes == bytes && std::memcmp(slot.copy.data(), code, bytes) == 0) return slot.hash;
    const uint64_t result = ShaderHashSlow(code, dwords);
    slot.code = code;
    slot.bytes = bytes;
    slot.copy.assign(code, code + bytes);
    slot.hash = result;
    return result;
  }
  struct ShaderSlot {
    const uint8_t* code = nullptr;
    size_t bytes = 0;
    std::vector<uint8_t> copy;
    uint64_t hash = 0;
  };
  ShaderSlot shader_slots_[256];

  uint64_t ShaderHashSlow(const uint8_t* code, uint32_t dwords) {
    const size_t bytes = size_t(dwords) * 4;
    const uint64_t fast = HashMemory(code, bytes, false);
    auto& known = shader_hashes_[fast];
    if (known.code.size() == bytes && std::memcmp(known.code.data(), code, bytes) == 0) return known.hash;
    if (shader_hashes_.size() > 8192) {
      shader_hashes_.clear();
      return HashWords(code, dwords);
    }
    known.code.assign(code, code + bytes);
    known.hash = HashWords(code, dwords);
    return known.hash;
  }

  uint64_t last_noted_[2] = {};
  void NoteShader(uint32_t type, const uint8_t* code, uint32_t dwords) {
    ProfileScope scope(kProfShaderHash);
    ++census_.shader_loads;
    census_.largest_shader_dwords = std::max(census_.largest_shader_dwords, dwords);
    const uint64_t hash = ShaderHash(code, dwords);
    (type == 1 ? capture_.pixel_shader : capture_.vertex_shader) = hash;
    // Only a shader the build does not have is worth a write to the SD card.
      if (hash != last_noted_[type & 1] && capture_.saved_shaders.insert(hash).second && !presenter_.FindShader(hash)) {
        SaveShader(type, hash, code, dwords);
      }
      last_noted_[type & 1] = hash;
  }

  const std::string& CaptureRoot() {
    if (capture_.root.empty()) {
      capture_.root = (rex::filesystem::GetExecutableFolder() / "capture").string();
      mkdir(capture_.root.c_str(), 0777);
    }
    return capture_.root;
  }

  void SaveShader(uint32_t type, uint64_t hash, const uint8_t* code, uint32_t dwords) {
    const std::string path =
        fmt::format("{}/{}_{:016x}.bin", CaptureRoot(), type == 1 ? "ps" : "vs", hash);
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(code, 4, dwords, f);
      std::fclose(f);
    }
  }

  // Stale GPU state can name memory the game has since freed, and reading that would crash.
  bool PhysicalRangeMapped(uint32_t address, uint32_t bytes) {
    const uint32_t first = address & ~0xFFFu;
    if (page_checked_.empty()) page_checked_.resize(0x20000);
    for (uint64_t page = first; page < uint64_t(address) + bytes; page += 0x1000) {
      uint32_t& checked = page_checked_[uint32_t(page >> 12) & 0x1FFFF];
      if (checked == page_generation_) continue;
      bool mapped = false;
      for (uint32_t view : {0xA0000000u, 0xC0000000u, 0xE0000000u}) {
        const uint32_t guest = view + uint32_t(page);
        auto* heap = memory_->LookupHeap(guest);
        uint32_t protect = 0;
        if (heap && heap->QueryProtect(guest, &protect) && protect) {
          mapped = true;
          break;
        }
      }
      if (!mapped) return false;
      checked = page_generation_;
    }
    return true;
  }

  void CaptureMemory(uint32_t kind, uint32_t address, uint32_t bytes) {
    address &= 0x1FFFFFFF;
    if (!bytes || bytes > kCaptureLargestRange) return;
    if (!capture_.saved_ranges.insert({address, bytes}).second) return;
    if (capture_.memory_bytes + bytes > kCaptureMemoryLimit ||
        (kind != 2 && capture_.geometry_bytes + bytes > kCaptureGeometryLimit) || !PhysicalRangeMapped(address, bytes)) {
      ++capture_.skipped_ranges;
      return;
    }
    if (kind != 2) capture_.geometry_bytes += bytes;
    capture_.Put32(kTagMemory);
    capture_.Put32(kind);
    capture_.Put32(address);
    capture_.Put32(bytes);
    const size_t at = capture_.out.size();
    capture_.out.resize(at + ((size_t(bytes) + 3) & ~size_t(3)));
    std::memcpy(&capture_.out[at], memory_->TranslatePhysical(address), bytes);
    capture_.memory_bytes += bytes;
  }

  void CaptureTexture(uint32_t first) {
    const uint32_t word0 = registers_[first], word1 = registers_[first + 1], word2 = registers_[first + 2];
    const uint32_t format = word1 & 0x3F;
    const uint32_t width = (word2 & 0x1FFF) + 1, height = ((word2 >> 13) & 0x1FFF) + 1;
    uint32_t pitch = ((word0 >> 22) & 0x1FF) << 5;
    if (!pitch) pitch = (width + 31) & ~31u;
    uint32_t bytes;
    if (format >= 18 && format <= 20) {  // DXT1, DXT3, DXT5
      const uint32_t rows = (((height + 3) / 4) + 31) & ~31u;
      bytes = (pitch / 4) * rows * (format == 18 ? 8 : 16);
    } else {
      const uint32_t rows = (height + 31) & ~31u;
      bytes = pitch * rows * (format == 2 ? 1 : 4);
    }
    CaptureMemory(2, (word1 >> 12) << 12, (bytes + 0xFFF) & ~0xFFFu);
  }

  void CaptureDraw(uint32_t initiator, bool predicated, uint32_t dma_base, uint32_t dma_size) {
    Capture& c = capture_;
    ++c.captured_draws;
    c.Put32(kTagDraw);
    c.Put32(initiator);
    c.Put32(predicated);
    c.Put32(dma_base);
    c.Put32(dma_size);
    c.Put64(c.vertex_shader);
    c.Put64(c.pixel_shader);
    const size_t count_at = c.out.size();
    c.Put32(0);
    uint32_t changed = 0;
    for (uint32_t r = 0; r < kRegisterCount; ++r) {
      if (registers_[r] != c.previous[r]) {
        c.Put32(r);
        c.Put32(registers_[r]);
        c.previous[r] = registers_[r];
        ++changed;
      }
    }
    std::memcpy(&c.out[count_at], &changed, 4);
    if (((initiator >> 6) & 3) == 0 && dma_base) CaptureMemory(0, dma_base, (initiator >> 16) * 4);
    for (uint32_t r = kRegFetchFirst; r < kRegFetchLast; r += 2) {
      if ((registers_[r] & 3) == 3) {
        CaptureMemory(1, registers_[r] & ~3u, ((registers_[r + 1] >> 2) & 0xFFFFFF) * 4);
      }
    }
    for (uint32_t r = kRegFetchFirst; r + 5 <= kRegFetchLast; r += 6) {
      if ((registers_[r] & 3) == 2) CaptureTexture(r);
    }
  }

  // One line per draw, for frames whose draw list differs from the frame before: it shows what a
  // frame with an effect has that the frames around it do not.
  void NoteDrawInLog(uint32_t initiator, bool predicated) {
    if (!capture_.active || draw_log_.bytes >= kDrawLogLimit) return;
    const uint32_t surface = Register(kRegRbSurfaceInfo), mask = Register(kRegRbColorMask) & 0xF;
    const uint32_t mode = Register(kRegRbModeControl) & 7, depth = Register(0x2200);
    const uint32_t word0 = registers_[kRegFetchFirst], word1 = registers_[kRegFetchFirst + 1];
    const uint32_t word2 = registers_[kRegFetchFirst + 2];
    const uint32_t texture = (word1 >> 12) << 12, format = word1 & 0x3F;
    const uint32_t ps = uint32_t(capture_.pixel_shader), vs = uint32_t(capture_.vertex_shader);
    const uint32_t blend = Register(0x2201), stencil = Register(kRegRbStencilRefMask);
    const uint32_t parts[] = {uint32_t(skip_), mode, surface, mask, ps, vs, initiator, texture, format, blend};
    for (const uint32_t part : parts) draw_log_.hash = (draw_log_.hash ^ part) * 0x100000001B3ull;
    draw_log_.frame += fmt::format(
        "{} skip{} em{} pitch{} aa{} wm{:X} pr{} ps{:08x} vs{:08x} n{} pt{} src{} tex{:08x} f{} {}x{} t{} bl{:08x} "
        "dc{:08x} st{:08x}\n",
        draw_log_.count++, int(skip_), mode, surface & 0x3FFF, 1u << ((surface >> 16) & 3), mask,
        predicated ? 1 : 0, ps, vs, initiator >> 16, initiator & 0x3F, (initiator >> 6) & 3, texture, format,
        (word2 & 0x1FFF) + 1, ((word2 >> 13) & 0x1FFF) + 1, word0 >> 31, blend, depth, stencil);
  }

  void EndDrawLogFrame() {
    DrawLog& log = draw_log_;
    const bool changed = log.hash != log.previous_hash;
    log.flash = changed && log.same_frames >= kCaptureStableFrames;
    log.same_frames = changed ? 0 : log.same_frames + 1;
    log.previous_hash = log.hash;
    log.hash = 0xCBF29CE484222325ull;
    if (!log.frame.empty()) {
      const std::string path = CaptureRoot() + "/drawlog.txt";
      if (FILE* f = std::fopen(path.c_str(), log.bytes ? "ab" : "wb")) {
        const std::string head = fmt::format("# swap {} draws {} flash {}\n", census_.total_swaps, log.count,
                                             log.flash ? 1 : 0);
        std::fwrite(head.data(), 1, head.size(), f);
        std::fwrite(log.frame.data(), 1, log.frame.size(), f);
        std::fclose(f);
        log.bytes += head.size() + log.frame.size();
      }
    }
    log.frame.clear();
    log.count = 0;
  }

  // When frames stop, each thread is paused for a moment and its pc, lr and the code addresses on its
  // stack are copied out. Recompiled code uses x29 as a general register, so frame pointers cannot be
  // trusted, and the stack scan gives the call chain instead. Nothing may allocate or log while a thread
  // is paused: it could be holding the lock that would need.
  void DumpThreads() {
    struct Stack {
      std::string name;
      uint32_t id = 0, handle = 0, count = 0;
      bool ok = false;
      uint64_t pc = 0, lr = 0, sp = 0;
      std::array<uint64_t, 40> returns{};
    };
    const uint64_t image = reinterpret_cast<uint64_t>(&_start);
    MemoryInfo text;
    u32 page_info = 0;
    if (R_FAILED(svcQueryMemory(&text, &page_info, image))) return;
    const uint64_t text_end = text.addr + text.size;
    const uint32_t self = RexSwitchHandleFor(pthread_self());
    auto threads = kernel_state_->object_table()->GetObjectsByType<rex::system::XThread>();
    std::vector<Stack> stacks;
    stacks.reserve(threads.size());
    for (auto& thread : threads) {
      if (!thread->thread()) continue;
      const uint32_t handle = RexSwitchHandleFor(reinterpret_cast<pthread_t>(thread->thread()->native_handle()));
      if (!handle || handle == self) continue;
      Stack stack;
      stack.name = thread->thread()->name();
      stack.id = thread->thread_id();
      stack.handle = handle;
      stacks.push_back(std::move(stack));
    }
    for (Stack& stack : stacks) {
      if (R_FAILED(svcSetThreadActivity(stack.handle, ThreadActivity_Paused))) continue;
      ThreadContext context;
      Result result = svcGetThreadContext3(&context, stack.handle);
      for (uint32_t tries = 0; R_FAILED(result) && tries < 20; ++tries) {
        svcSleepThread(100000);
        result = svcGetThreadContext3(&context, stack.handle);
      }
      if (R_SUCCEEDED(result)) {
        stack.ok = true;
        stack.pc = context.pc.x;
        stack.lr = context.lr;
        stack.sp = context.sp;
        MemoryInfo region;
        if (R_SUCCEEDED(svcQueryMemory(&region, &page_info, context.sp)) && (region.perm & Perm_R)) {
          const uint64_t end = std::min<uint64_t>(region.addr + region.size, context.sp + 0x8000);
          for (uint64_t at = context.sp & ~uint64_t(7); at + 8 <= end && stack.count < stack.returns.size();
               at += 8) {
            const uint64_t value = *reinterpret_cast<const uint64_t*>(at);
            if (value < text.addr + 0x100 || value >= text_end) continue;
            if (stack.count && stack.returns[stack.count - 1] == value) continue;
            stack.returns[stack.count++] = value;
          }
        }
      }
      svcSetThreadActivity(stack.handle, ThreadActivity_Runnable);
    }
    REXLOG_ERROR("[stall] no frames for 10 s; {} threads, image {:016x}", stacks.size(), image);
    for (const Stack& stack : stacks) {
      std::string text_line;
      for (uint32_t i = 0; i < stack.count; ++i) text_line += fmt::format(" {:x}", stack.returns[i] - image);
      REXLOG_ERROR("[stall] t{:x} '{}' {}: pc {:x} lr {:x} stack{}", stack.id, stack.name, stack.ok ? "ok" : "no context",
                   stack.pc - image, stack.lr - image, text_line);
    }
  }

  void CaptureFrameEnd() {
    Capture& c = capture_;
    const uint32_t frame_draws = c.frame_draws;
    c.frame_draws = 0;
    if (c.active) {
      c.Put32(kTagSwap);
      c.Put32(census_.frontbuffer);
      c.Put32(census_.front_width);
      c.Put32(census_.front_height);
      const std::string path = fmt::format("{}/frame_{}.bin", CaptureRoot(), c.frames_done);
      bool written = false;
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        written = std::fwrite(c.out.data(), 1, c.out.size(), f) == c.out.size();
        std::fclose(f);
      }
      REXLOG_INFO("[capture] {} {}: {} draws, {:.1f} MB, {:.1f} MB of guest memory, {} ranges skipped",
                  written ? "wrote" : "FAILED to write", path, c.captured_draws,
                  double(c.out.size()) / 1048576.0, double(c.memory_bytes) / 1048576.0, c.skipped_ranges);
      c.active = false;
      ++c.frames_done;
      c.next_swap = census_.total_swaps + kCaptureGapSwaps;
      std::vector<uint8_t>().swap(c.out);
      c.saved_ranges.clear();
      return;
    }
    // Only frames with many off-screen passes: those are the 3D scenes the renderer cannot draw yet.
    const uint32_t frame_resolves = c.frame_resolves;
    c.frame_resolves = 0;
    // A run of heavy frames, not one: a spike as a scene starts is followed by a loading screen.
    const bool heavy = frame_draws >= kCaptureMinDraws && frame_resolves >= kCaptureMinResolves;
    c.heavy_frames = heavy ? c.heavy_frames + 1 : 0;
    if (kSurfaceDumps && c.heavy_frames >= kCaptureHeavyFrames && dumps_done_ < 2 &&
        census_.total_swaps >= next_dump_swap_) {
      dump_requested_ = true;
    }
    // Test runs only: a marker file captures the frames around each piece of a Sim's atlas.
    const bool atlas_mode = AtlasMode();
    // Test runs only: frames at fixed points, for screens that are never heavy, such as the main menu.
    static const bool timed_mode = [&] {
      struct stat marker;
      return stat((CaptureRoot() + "/timed.on").c_str(), &marker) == 0;
    }();
    // Test aid: both sticks pressed together capture the next frame, for problems no marker can predict.
    if (census_.total_swaps > 600) {
      if (!pad_ready_) {
        padInitializeAny(&capture_pad_);
        pad_ready_ = true;
      }
      padUpdate(&capture_pad_);
      const uint64_t held = padGetButtons(&capture_pad_);
      const bool combo = (held & HidNpadButton_StickL) && (held & HidNpadButton_StickR);
      if (combo && !combo_held_ && manual_captures_ < 4) {
        ++manual_captures_;
        REXLOG_INFO("[capture] L3+R3 pressed, capturing the next frame");
        combo_held_ = combo;
        StartCapture();
        return;
      }
      combo_held_ = combo;
    }
    if (world_capture_next_ && !c.active) {
      world_capture_next_ = false;
      ++world_captures_;
      REXLOG_INFO("[capture] world frame with a copied Sim atlas");
      StartCapture();
      return;
    }
    const bool atlas_changed = presenter_.atlas_events() != atlas_events_seen_;
    atlas_events_seen_ = presenter_.atlas_events();
    if (timed_mode ? (census_.total_swaps < 3000 || census_.total_swaps % 450 != 0 || c.frames_done >= 6)
        : atlas_mode ? true  // begins inside the frame, in ResolveDraw
                   : (c.frames_done >= kCaptureFrames || census_.total_swaps < c.next_swap ||
                      c.heavy_frames < kCaptureHeavyFrames)) {
      return;
    }
    StartCapture();
  }

  void StartCapture() {
    Capture& c = capture_;
    c.active = true;
    c.captured_draws = c.skipped_ranges = 0;
    c.memory_bytes = 0;
    c.geometry_bytes = 0;
    c.previous = registers_;
    c.out.reserve(size_t(32) << 20);
    c.Put32(0x46433353);  // 'S3CF'
    c.Put32(1);           // format version
    c.Put32(kTagRegisters);
    c.Put32(kRegisterCount);
    const size_t at = c.out.size();
    c.out.resize(at + size_t(kRegisterCount) * 4);
    std::memcpy(&c.out[at], registers_.data(), size_t(kRegisterCount) * 4);
  }

  void Report() {
    Census& c = census_;
    const auto now = Clock::now();
    if (now - c.last < kCensusSummary) return;
    const double seconds = std::chrono::duration<double>(now - c.last).count();
    const auto ms = [](uint64_t ns) { return double(ns) / 1e6; };
    const double per_frame = c.swaps ? double(c.draws) / double(c.swaps) : 0.0;
    REXLOG_INFO(
        "[census] {:.1f} s: {:.1f} fps ({} swaps), {} draws ({:.0f}/frame), {} resolves, {} predicated, "
        "{} indices; ring {} KW, {} IBs, {} interrupts; waits {} ({} timed out, {:.0f} ms); ring busy "
        "{:.0f} ms, present {:.0f} ms",
        seconds, double(c.swaps) / seconds, c.swaps, c.draws, per_frame, c.resolves, c.predicated,
        c.indices, c.ring_words / 1000, c.indirect_buffers, c.interrupts, c.waits, c.wait_timeouts,
        ms(c.wait_ns), ms(c.busy_ns), ms(c.present_ns));
    const auto submits = presenter_.TakeSubmitCounts();
    {
      const auto ms = [](uint64_t ticks) { return double(armTicksToNs(ticks)) / 1e6; };
      REXLOG_INFO("[profile] ms per 5 s: vertex {:.0f} ({:.0f} MB), index {:.0f}, texture {:.0f} (hash {:.0f} over {:.0f} MB, "
                  "upload {:.0f}: {} of {:.0f} MB), constants {:.0f}, record {:.0f} (submit {:.0f}), resolve {:.0f}, mid-frame waits {:.0f}, "
                  "clear {:.0f}; all draws {:.0f}, shader hashing {:.0f}, ring total {:.0f} (with waits); vertex cache "
                  "{} rested, {} rechecked, {} rewritten, {:.1f} MB; waiting for the frame before last {:.0f}",
                  ms(g_profile[kProfVertex]), double(g_vertex_bytes) / 1048576.0, ms(g_profile[kProfIndex]),
                  ms(g_profile[kProfTexture]), ms(g_profile[kProfHash]), double(g_hash_bytes) / 1048576.0,
                  ms(g_profile[kProfUpload]), g_uploads, double(g_upload_bytes) / 1048576.0, ms(g_profile[kProfConstants]), ms(g_profile[kProfRecord]),
                  ms(g_profile[kProfSubmit]), ms(g_profile[kProfResolve]), ms(g_profile[kProfFlush]),
                  ms(g_profile[kProfClear]), ms(g_profile[kProfDraw]), ms(g_profile[kProfShaderHash]),
                  ms(g_profile[kProfPacket]), g_vcache[0], g_vcache[1], g_vcache[2],
                  double(presenter_.vertex_cache_used()) / 1048576.0, ms(g_profile[kProfFence]));
      {
        std::vector<std::pair<uint32_t, TextureStat>> rows(g_texture_stats.begin(), g_texture_stats.end());
        const auto line = [](const std::pair<uint32_t, TextureStat>& r) {
          return fmt::format(" {:08X} {}x{} f{}{}{}: {} up {:.1f} MB, {} checks {:.0f} MB", r.first, r.second.width,
                             r.second.height, r.second.format, r.second.tiled ? " tiled" : "", r.second.full ? " ui" : "",
                             r.second.uploads, double(r.second.uploaded) / 1048576.0, r.second.checks,
                             double(r.second.hashed) / 1048576.0);
        };
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.uploaded > b.second.uploaded; });
        std::string top;
        for (size_t i = 0; i < rows.size() && i < 8 && rows[i].second.uploads; ++i) top += line(rows[i]) + ";";
        REXLOG_INFO("[textures] most uploaded:{}", top);
        REXLOG_INFO("[textures] written back:{}", presenter_.TakeWritebackStats());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.hashed > b.second.hashed; });
        top.clear();
        for (size_t i = 0; i < rows.size() && i < 8; ++i) top += line(rows[i]) + ";";
        REXLOG_INFO("[textures] most hashed ({} textures checked):{}", rows.size(), top);
        g_texture_stats.clear();
      }
      g_vcache[0] = g_vcache[1] = g_vcache[2] = 0;
      std::fill(std::begin(g_profile), std::end(g_profile), 0);
      g_vertex_bytes = g_hash_bytes = g_upload_bytes = g_uploads = 0;
    }
    REXLOG_INFO("[render] {} drawn, {} 3D, {} host resolves, {}+{} depth+colour restores, {} untranslated, {} "
                "without a texture; {} textures, {:.1f} MB; {} targets, {} surfaces, {:.1f} MB frame memory; "
                "{} chunks, {} GPU waits mid-frame",
                ui_.drawn, ui_.generic, c.host_resolves, c.depth_restores, c.colour_restores, ui_.unknown,
                ui_.no_texture,
                presenter_.texture_count(),
                double(presenter_.texture_memory_used()) / 1048576.0, presenter_.target_count(),
                presenter_.surface_count(), double(presenter_.frame_memory_used()) / 1048576.0,
                submits.first, submits.second);
    idle_reports_ = c.swaps ? 0 : idle_reports_ + 1;
    if (idle_reports_ >= 2 && idle_reports_ % 2 == 0 && thread_dumps_ < 3) {
      ++thread_dumps_;
      DumpThreads();
    }
    ui_.drawn = ui_.unknown = ui_.no_texture = ui_.generic = 0;
    c.host_resolves = c.depth_restores = c.colour_restores = 0;
    c.last = now;
    c.swaps = c.draws = c.resolves = c.predicated = c.indices = 0;
    c.ring_words = c.indirect_buffers = c.interrupts = 0;
    c.waits = c.wait_timeouts = c.wait_ns = c.busy_ns = c.present_ns = 0;
    if (++c.reports % kCensusDetailEvery != 1) return;

    REXLOG_INFO("[census] totals: {} swaps, {} draws, {} resolves; {} vertex and {} pixel shaders from {} "
                "loads, largest {} dwords; {} texture addresses, largest {}x{}",
                c.total_swaps, c.total_draws, c.total_resolves, c.vertex_shaders.size(),
                c.pixel_shaders.size(), c.shader_loads, c.largest_shader_dwords, c.textures.size(),
                c.largest_texture_w, c.largest_texture_h);
    std::string text;
    for (uint32_t i = 0; i < c.primitive.size(); ++i) {
      if (c.primitive[i]) text += fmt::format(" {:#x}={}", i, c.primitive[i]);
    }
    REXLOG_INFO("[census] primitives:{}; source dma={} immediate={} auto={}", text, c.source[0],
                c.source[1], c.source[2]);
    text.clear();
    for (uint32_t i = 0; i < c.texture_format.size(); ++i) {
      if (c.texture_format[i]) text += fmt::format(" {}={}", i, c.texture_format[i]);
    }
    REXLOG_INFO("[census] texture formats:{}", text);
    std::vector<std::pair<uint64_t, uint32_t>> ranked;
    for (const auto& [key, count] : c.targets) ranked.push_back({count, key});
    std::sort(ranked.rbegin(), ranked.rend());
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 8); ++i) {
      const uint32_t key = ranked[i].second;
      REXLOG_INFO("[census] target: {} draws, pitch {}, msaa {}, colour format {}, depth format {}, {} MRT",
                  ranked[i].first, key & 0x3FFF, 1u << ((key >> 14) & 3), (key >> 16) & 0xF,
                  (key >> 20) & 1, key >> 21);
    }
    std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> sites;
    for (const auto& [key, site] : c.wait_sites) sites.push_back({site.ns, key});
    std::sort(sites.rbegin(), sites.rend());
    for (size_t i = 0; i < std::min<size_t>(sites.size(), 5); ++i) {
      const auto& site = c.wait_sites[sites[i].second];
      REXLOG_INFO("[census] wait {:08X} ref {:08X}: {} waits, {} timed out, {:.0f} ms",
                  sites[i].second.first, sites[i].second.second, site.count, site.timeouts, ms(site.ns));
    }
    text.clear();
    for (uint32_t i = 0; i < c.opcode.size(); ++i) {
      if (c.opcode[i]) text += fmt::format(" {:02X}={}", i, c.opcode[i]);
    }
    REXLOG_INFO("[census] PM4 opcodes:{}", text);
  }

  // Test runs only: the Sim's textures are built in one burst inside a single frame, so a marker file makes the
  // capture start at the first resolve of that burst rather than on the next frame.
  bool world_capture_next_ = false;
  std::unordered_map<uint32_t, uint32_t> target_msaa_;  // EDRAM target key -> sample count of its last non-4x draw
  uint32_t portrait_captures_ = 0;
  PadState capture_pad_{};
  bool pad_ready_ = false, combo_held_ = false;
  uint32_t manual_captures_ = 0;
  uint32_t world_captures_ = 0;

  bool AtlasMode() {
    static const bool mode = [&] {
      struct stat marker;
      return stat((CaptureRoot() + "/atlas.on").c_str(), &marker) == 0;
    }();
    return mode;
  }

  void BeginFrame() {
    if (!guest_memory_set_) {
      guest_memory_set_ = true;
      presenter_.SetGuestMemory([this](uint32_t address, uint32_t bytes) -> const uint8_t* {
        return PhysicalRangeMapped(address, bytes) ? memory_->TranslatePhysical(address) : nullptr;
      });
      // Test runs only: a marker file saves every atlas-sized resolve, so a build can be followed step by step.
      struct stat marker;
      if (stat((CaptureRoot() + "/steps.on").c_str(), &marker) == 0) presenter_.SetStepRoot(CaptureRoot());
      if (stat((CaptureRoot() + "/portrait.on").c_str(), &marker) == 0) presenter_.SetPortraitRoot(CaptureRoot());
      if (stat((CaptureRoot() + "/stencil.on").c_str(), &marker) == 0) presenter_.SetStencilRoot(CaptureRoot());
      presenter_.SetIconRoot(CaptureRoot());  // icon dumps: 24 files of about 17 KB
    }
    presenter_.Begin(0.0f, 0.0f, 0.0f);
  }

  void Present() {
    if (!presenting_) return;
    BeginFrame();
    if (probe_.state == 1) {
      ProbeSnapshot("at present, target");
      probe_.target = nullptr;  // then the front buffer the picture is shown from, at the same place
      probe_.last.clear();
      REXLOG_INFO("[probe] front buffer {:08X}", census_.frontbuffer);
      ProbeSnapshot("at present, front buffer");
      probe_.state = 2;
      if (probe_.file) std::fclose(probe_.file);
      probe_.file = nullptr;
    }
    presenter_.Present(census_.frontbuffer);
    texture_memo_.clear();
    frame_buffers_.clear();
    // Test runs only: a marker file in capture/ saves the main picture every 150 frames.
    if (census_.total_swaps % 150 == 0) {
      struct stat marker;
      if (stat((CaptureRoot() + "/shots.on").c_str(), &marker) == 0) {
        presenter_.Dump(fmt::format("{}/shot{:04}", CaptureRoot(), census_.total_swaps / 150), true);
      }
    }
    ++page_generation_;
    constant_bank_[0].valid = constant_bank_[1].valid = false;
    if (dump_requested_) {
      dump_requested_ = false;
      presenter_.Dump(fmt::format("{}/dump{}", CaptureRoot(), dumps_done_++));
      next_dump_swap_ = census_.total_swaps + 900;
    }
  }

  float Constant(bool pixel, uint32_t index, uint32_t component) const {
    return std::bit_cast<float>(registers_[(pixel ? 0x4400 : 0x4000) + index * 4 + component]);
  }

  void Constant4(bool pixel, uint32_t index, float* out) const {
    for (uint32_t i = 0; i < 4; ++i) out[i] = Constant(pixel, index, i);
  }

  float Dot(const float* r, uint32_t constant) const {
    float sum = 0;
    for (uint32_t i = 0; i < 4; ++i) sum += r[i] * Constant(false, constant, i);
    return sum;
  }

  struct VertexLayout {
    uint64_t hash;
    uint32_t slot, stride;
    enum Kind { kInt16Colours, kInt16Colour, kInt16, kFloat4Uv, kFloat4UvColour, kRawUv, kFloat3Colour, kFloat2 } kind;
  };

  static const VertexLayout* FindVertexLayout(uint64_t hash) {
    static constexpr VertexLayout kLayouts[] = {
        {0x92e80366cb1c386bull, 95, 3, VertexLayout::kInt16Colours},
        {0x7f64754ac3dde353ull, 95, 2, VertexLayout::kInt16Colour},
        {0x5db89eb8c07cc32full, 95, 1, VertexLayout::kInt16},
        {0x6dbe72b7b49efab7ull, 95, 7, VertexLayout::kFloat4Uv},
        {0xae95319b2aa28a09ull, 95, 7, VertexLayout::kFloat4UvColour},
        {0xcfc2fa687333e466ull, 95, 6, VertexLayout::kRawUv},
        {0x539ea9e08aa83f0cull, 0, 7, VertexLayout::kFloat3Colour},
        {0xa4ae7c2b7c1818c4ull, 0, 2, VertexLayout::kFloat2},
    };
    for (const auto& layout : kLayouts) {
      if (layout.hash == hash) return &layout;
    }
    return nullptr;
  }

  static int FindPixelMode(uint64_t hash) {
    switch (hash) {
      case 0x438c2af84c78a133ull: return 0;
      case 0x03119cfd11beb173ull: return 1;
      case 0x7a27a638de062231ull: return 2;
      case 0xa15c6c9c2d249375ull: return 3;
      case 0x1d03653c7a4cff4bull: return 4;
      case 0x43f1e01c2eb935beull: return 5;
      case 0x4d80b8b337ff38f4ull: return 6;
      default: return -1;
    }
  }

  static void Colour(uint32_t word, float* out) {
    for (uint32_t i = 0; i < 4; ++i) out[i] = float((word >> (8 * i)) & 255) / 255.0f;
  }

  // The same decoding as tools/capsim.py, which was checked against captured frames.
  void DecodeVertex(const VertexLayout& layout, const uint8_t* base, uint32_t endian, uint32_t index,
                    uint32_t limit_words, UiVertex& v) {
    float position[4] = {0, 0, 0, 1};
    v.u = v.v = 0;
    for (uint32_t i = 0; i < 4; ++i) v.c0[i] = v.c1[i] = 1.0f;
    const uint32_t first = index * layout.stride;
    if (first + layout.stride > limit_words) {
      v.x = v.y = 0;
      return;
    }
    const auto word = [&](uint32_t i) {
      uint32_t value;
      std::memcpy(&value, base + (first + i) * 4, 4);
      return GpuSwap(value, endian);
    };
    const auto real = [&](uint32_t i) { return std::bit_cast<float>(word(i)); };
    switch (layout.kind) {
      case VertexLayout::kInt16Colours:
      case VertexLayout::kInt16Colour:
      case VertexLayout::kInt16: {
        const uint32_t packed = word(0);
        // Component 0 is the low half and the fetch swizzle is yx01.
        const float r[4] = {float(int16_t(packed >> 16)), float(int16_t(packed & 0xFFFF)), 0, 1};
        position[0] = Dot(r, 0);
        position[1] = Dot(r, 1);
        if (layout.kind == VertexLayout::kInt16Colours) {
          Colour(word(1), v.c0);
          Colour(word(2), v.c1);
          v.u = Dot(r, 4);
          v.v = Dot(r, 5);
        } else if (layout.kind == VertexLayout::kInt16Colour) {
          Colour(word(1), v.c0);
        }
        break;
      }
      case VertexLayout::kFloat4Uv:
      case VertexLayout::kFloat4UvColour: {
        const float r[4] = {real(0), real(1), real(2), real(3)};
        position[0] = Dot(r, 0);
        position[1] = Dot(r, 1);
        position[2] = r[2];
        position[3] = r[3];
        v.u = real(4);
        v.v = real(5);
        if (layout.kind == VertexLayout::kFloat4UvColour) {
          float c[4];
          Colour(word(6), c);
          v.c0[0] = c[2];
          v.c0[1] = c[1];
          v.c0[2] = c[0];
          v.c0[3] = c[3];
        }
        break;
      }
      case VertexLayout::kRawUv:
        for (uint32_t i = 0; i < 4; ++i) position[i] = real(i);
        v.u = real(4);
        v.v = real(5);
        break;
      case VertexLayout::kFloat3Colour:
        position[0] = real(0);
        position[1] = real(1);
        position[2] = real(2);
        for (uint32_t i = 0; i < 4; ++i) v.c0[i] = real(3 + i);
        break;
      case VertexLayout::kFloat2:
        position[0] = real(0);
        position[1] = real(1);
        break;
    }
    const uint32_t control = Register(0x2206);
    float x = position[0], y = position[1];
    if (position[3] != 0.0f) {
      x /= position[3];
      y /= position[3];
    }
    if (control & 1) x *= std::bit_cast<float>(Register(0x210F));
    if (control & 2) x += std::bit_cast<float>(Register(0x2110));
    if (control & 4) y *= std::bit_cast<float>(Register(0x2111));
    if (control & 8) y += std::bit_cast<float>(Register(0x2112));
    v.x = x * 2.0f / float(kWidth) - 1.0f;
    v.y = 1.0f - y * 2.0f / float(kTargetHeight);  // deko3d's clip space has +Y at the top
  }

  uint32_t mip_logs_ = 0;

  bool FindTexture(Deko3dPresenter::Texture* out, uint32_t first = kRegFetchFirst, bool cube = false,
                   bool full_hash = false) {
    const uint32_t word0 = registers_[first], word1 = registers_[first + 1];
    const uint32_t word2 = registers_[first + 2], word3 = registers_[first + 3];
    TextureRequest r;
    r.key = {word0, word1, word2, word3 ^ (cube ? 0x80000000u : 0u)};
    r.cube = cube;
    r.full_hash = full_hash;
    std::array<uint32_t, 5> memo_key = {r.key[0], r.key[1], r.key[2], r.key[3], uint32_t(full_hash)};
    // Once per frame: the mapped check walks every page, and many draws share one texture.
    if (const auto hit = texture_memo_.find(memo_key); hit != texture_memo_.end()) {
      *out = hit->second;
      return out->valid;
    }
    r.signs = (word0 >> 2) & 0xFF;
    if (GammaFetch(r.signs) && !full_hash) r.key[3] ^= 0x40000000u;  // the interface keeps its plain copy
    r.format = word1 & 0x3F;
    if (r.format >= 51 && r.format <= 53) r.format -= 33;  // the _AS_16_16_16_16 forms hold plain DXT data
    r.endian = (word1 >> 6) & 3;
    r.width = (word2 & 0x1FFF) + 1;
    r.height = ((word2 >> 13) & 0x1FFF) + 1;
    r.pitch = ((word0 >> 22) & 0x1FF) << 5;
    if (!r.pitch) r.pitch = (r.width + 31) & ~31u;
    r.tiled = (word0 >> 31) != 0;
    for (uint32_t i = 0; i < 4; ++i) r.swizzle[i] = (word3 >> (1 + 3 * i)) & 7;
    if (!cube && ((registers_[first + 5] >> 11) & 1)) {
      uint32_t log2_width = 0, log2_height = 0;
      while ((1u << log2_width) < r.width) ++log2_width;
      while ((1u << log2_height) < r.height) ++log2_height;
      if (std::min(log2_width, log2_height) <= 4) {
        (log2_width > log2_height ? r.packed_y : r.packed_x) = 16;
        r.key[1] ^= 0x100u;  // request size bits, free for this
      }
    }
    const bool compressed = r.format >= 18 && r.format <= 20;
    const uint32_t block = compressed ? 4 : 1;
    const uint32_t bytes =
        r.format == 18 ? 8 : compressed ? 16 : r.format == 2 ? 1 : (r.format == 3 || r.format == 10) ? 2 : 4;
    const uint32_t rows = (((r.height + block - 1) / block) + 31) & ~31u;
    r.guest_bytes = (((r.pitch / block) * rows * bytes) + 0xFFF) & ~0xFFFu;
    if (cube) {  // each face starts on a 4 KB boundary
      r.slice_bytes = r.guest_bytes;
      r.guest_bytes *= 6;
    }
    const uint32_t base = (word1 >> 12) << 12;
    if (r.guest_bytes > kCaptureLargestRange || !PhysicalRangeMapped(base, r.guest_bytes)) return false;
    r.guest = memory_->TranslatePhysical(base);
    const uint32_t word4 = registers_[first + 4], word5 = registers_[first + 5];
    const uint32_t max_level = (word4 >> 6) & 0xF, mip_base = (word5 >> 12) << 12;
    if (kMipmaps && !cube && !full_hash && r.tiled && max_level && mip_base && !r.packed_x && !r.packed_y &&
        r.format != 3 && std::has_single_bit(r.width) && std::has_single_bit(r.height) &&
        std::min(r.width, r.height) >= 16) {
      uint32_t limit = uint32_t(std::countr_zero(std::min(r.width, r.height))) + 1;  // down to one texel
      if (compressed) limit -= 2;                                                   // down to one 4x4 block
      const uint32_t levels = std::min(max_level + 1, std::min(limit, 16u));
      GuestMip mips[16];
      const bool packed = (word5 >> 11) & 1;
      const uint32_t chain = GuestMips(r.format, r.width, r.height, levels, packed, mips);
      if (levels > 1 && chain && chain <= kCaptureLargestRange && PhysicalRangeMapped(mip_base, chain)) {
        r.mip_levels = levels;
        r.packed_mips = packed;
        r.mip_bytes = chain;
        r.mip_guest = memory_->TranslatePhysical(mip_base);
        r.key[1] ^= 0x200u;  // request size bits, free for this
        if (mip_logs_ < 24) {
          ++mip_logs_;
          REXLOG_INFO("[render] mips: {:08X} format {} {}x{}, {} levels (max {}), packed {}, chain at {:08X}, {} KB",
                      base, r.format, r.width, r.height, levels, max_level, packed, mip_base, chain >> 10);
        }
      }
    }
    *out = presenter_.FindTexture(r);
    texture_memo_[memo_key] = *out;
    return out->valid;
  }

  // Half icons, test runs: once, the pixels under the town HUD's bottom-left B icon are read back before and after
  // it is drawn and after every later draw of that frame. Each change is logged with the draw that made it, and the
  // pictures go to capture/probe.bin (per snapshot: index, width, height, then RGBA8 rows).
  static constexpr uint32_t kProbeX = 56, kProbeY = 550, kProbeW = 48, kProbeH = 44;
  struct IconProbe {
    int state = 0;  // 0 waiting, 1 running, 2 done
    uint32_t sightings = 0, index = 0, logs = 0;
    void* target = nullptr;
    std::vector<uint8_t> last;
    FILE* file = nullptr;
  } probe_;

  void ProbeSnapshot(const std::string& what) {
    std::vector<uint8_t> now;
    if (!presenter_.ReadTarget(probe_.target, census_.frontbuffer, kProbeX, kProbeY, kProbeW, kProbeH, &now)) return;
    const uint32_t index = probe_.index++;
    if (probe_.file) {
      const uint32_t header[3] = {index, kProbeW, kProbeH};
      std::fwrite(header, 4, 3, probe_.file);
      std::fwrite(now.data(), 1, now.size(), probe_.file);
    }
    // The icon covers about x 63-92, y 558-587: left half 63-77, right half 77-92.
    uint32_t changed_left = 0, changed_right = 0;
    double right[4] = {0, 0, 0, 0};
    for (uint32_t y = 558 - kProbeY; y < 587 - kProbeY; ++y) {
      for (uint32_t x = 63 - kProbeX; x < 92 - kProbeX; ++x) {
        const uint8_t* p = &now[(y * kProbeW + x) * 4];
        bool differs = probe_.last.empty();
        if (!differs) {
          const uint8_t* q = &probe_.last[(y * kProbeW + x) * 4];
          for (int c = 0; c < 4; ++c) differs |= std::abs(int(p[c]) - int(q[c])) > 6;
        }
        const bool is_right = x + kProbeX >= 77;
        if (differs) ++(is_right ? changed_right : changed_left);
        if (is_right) {
          for (int c = 0; c < 4; ++c) right[c] += p[c];
        }
      }
    }
    if ((changed_left || changed_right || index < 2) && probe_.logs < 200) {
      ++probe_.logs;
      const double n = 29.0 * 15.0;
      REXLOG_INFO("[probe] {} {}: changed left {} right {}, right mean {:.0f} {:.0f} {:.0f} {:.0f}", index, what,
                  changed_left, changed_right, right[0] / n, right[1] / n, right[2] / n, right[3] / n);
    }
    probe_.last = std::move(now);
  }

  std::string ProbeDraw(uint32_t initiator) const {
    const uint32_t word1 = registers_[kRegFetchFirst + 1], word2 = registers_[kRegFetchFirst + 2];
    return fmt::format("draw vs {:016x} ps {:016x} edram {} skip {} prim {} count {} tex {:08X} {}x{} blend {:08X} "
                       "mask {:X} depth {:08X} stencil {:08X} colourctl {:08X} surface {:08X} colourinfo {:08X} "
                       "scissor {:08X} {:08X} window {:08X}",
                       capture_.vertex_shader, capture_.pixel_shader, Register(kRegRbModeControl) & 7, int(skip_),
                       initiator & 0x3F, initiator >> 16, (word1 >> 12) << 12, (word2 & 0x1FFF) + 1,
                       ((word2 >> 13) & 0x1FFF) + 1, Register(0x2201), Register(kRegRbColorMask) & 0xF,
                       Register(kRegRbDepthControl), Register(kRegRbStencilRefMask), Register(0x2202),
                       Register(kRegRbSurfaceInfo), Register(kRegRbColorInfo), Register(0x2081), Register(0x2082),
                       Register(0x2080));
  }

  void RenderDraw(uint32_t initiator, bool predicated, uint32_t dma_base, uint32_t dma_size) {
    RenderDrawBody(initiator, predicated, dma_base, dma_size);
    if (probe_.state == 1 && probe_.index < 600) ProbeSnapshot(ProbeDraw(initiator));
  }

  void RenderDrawBody(uint32_t initiator, bool predicated, uint32_t dma_base, uint32_t dma_size) {
    skip_ = DrawSkip::kOther;
    if (!presenting_) return;
    if (predicated && !(bin_select_ & bin_mask_)) {
      skip_ = DrawSkip::kPredicate;
      return;
    }
    const uint32_t edram_mode = Register(kRegRbModeControl) & 7;
    if (edram_mode == kEdramModeCopy) {
      ResolveDraw();
      return;
    }
    if (edram_mode != 4 && edram_mode != 5) {
      skip_ = DrawSkip::kTarget;
      return;
    }
    if (ClearDraw(initiator)) {
      skip_ = DrawSkip::kDrawn;
      return;
    }
    const uint32_t write_mask = Register(kRegRbColorMask) & 0xF;
    const uint32_t depth_control = Register(kRegRbDepthControl);
    const VertexLayout* layout = FindVertexLayout(capture_.vertex_shader);
    const int mode = FindPixelMode(capture_.pixel_shader);
    const uint32_t surface_info = Register(kRegRbSurfaceInfo);
    // The tested interface path keeps the screen target. Everything else takes the translated shaders.
    const bool interface_draw = edram_mode == 4 && layout && mode >= 0 && (surface_info & 0x3FFF) == kWidth &&
                                ((surface_info >> 16) & 3) == 0 && (write_mask || (depth_control & 1));
    // Test runs only: a mesh drawn into a small off-screen target is a portrait or an outfit icon, made once when a
    // panel opens, so no button press can catch it. The capture starts at that draw.
    // Keyed on what is drawn, not the target: a headshot goes into the big target through a small viewport or
    // scissor, and it may write colour only (its alpha is the transparent background). Checked against eight
    // earlier captures: nothing in ordinary frames matches (64-pixel cube faces and 1-channel passes are left out).
    const float viewport_half_width = std::fabs(std::bit_cast<float>(Register(0x210F)));
    const uint32_t scissor_tl = Register(0x2081), scissor_br = Register(0x2082);
    const int32_t scissor_w = int32_t(scissor_br & 0x7FFF) - int32_t(scissor_tl & 0x7FFF);
    const int32_t scissor_h = int32_t((scissor_br >> 16) & 0x7FFF) - int32_t((scissor_tl >> 16) & 0x7FFF);
    const bool small_view = (viewport_half_width >= 40.0f && viewport_half_width <= 260.0f) ||
                            (scissor_w >= 96 && scissor_w <= 520 && scissor_h >= 96 && scissor_h <= 520);
    if (!interface_draw && !capture_.active && edram_mode == 4 && (write_mask & 7) == 7 && portrait_captures_ < 2 &&
        (initiator >> 16) >= 100 && small_view && AtlasMode()) {
      ++portrait_captures_;
      REXLOG_INFO("[capture] mesh drawn through a {}-wide viewport, scissor {}x{}, mask {:X}, target {} wide (msaa {}), "
                  "capturing from here",
                  viewport_half_width * 2.0f, scissor_w, scissor_h, write_mask, surface_info & 0x3FFF,
                  1u << ((surface_info >> 16) & 3));
      StartCapture();
    }
    if (!interface_draw) {
      RenderGeneric(initiator, dma_base, dma_size, write_mask, depth_control);
      return;
    }
    const uint32_t primitive = initiator & 0x3F, source = (initiator >> 6) & 3, count = initiator >> 16;
    if ((primitive != 4 && primitive != 8) || source == 1 || !count) {
      ++ui_.unknown;
      return;
    }
    const uint32_t fetch = kRegFetchFirst + layout->slot * 2;
    const uint32_t vertex_address = registers_[fetch] & 0x1FFFFFFC;
    const uint32_t vertex_endian = registers_[fetch + 1] & 3;
    const uint32_t vertex_words = (registers_[fetch + 1] >> 2) & 0xFFFFFF;
    if (!vertex_words || !PhysicalRangeMapped(vertex_address, vertex_words * 4)) {
      ++ui_.unknown;
      return;
    }
    const uint8_t* vertex_data = memory_->TranslatePhysical(vertex_address);

    const uint8_t* index_data = nullptr;
    const bool index32 = (initiator >> 11) & 1;
    const uint32_t index_endian = (dma_size >> 30) & 3;
    if (source == 0) {
      const uint32_t address = dma_base & 0x1FFFFFFF;
      if (!PhysicalRangeMapped(address, count * (index32 ? 4 : 2))) {
        ++ui_.unknown;
        return;
      }
      index_data = memory_->TranslatePhysical(address);
    }
    const auto vertex_index = [&](uint32_t i) -> uint32_t {
      if (!index_data) return i;
      if (index32) {
        uint32_t value;
        std::memcpy(&value, index_data + i * 4, 4);
        return GpuSwap(value, index_endian);
      }
      uint16_t value;
      std::memcpy(&value, index_data + i * 2, 2);
      return index_endian == 1 ? std::byteswap(value) : value;
    };

    // Before FindTexture: starting the frame clears the upload list that FindTexture fills.
    BeginFrame();
    SetEdramTarget();
    Deko3dPresenter::Texture texture;
    const bool textured = mode >= 3;
    if (textured) {
      // The game renders effects and portraits off screen. Those exist only as surfaces: the bytes in guest
      // memory are not written, and reading them there gives the black shapes.
      const uint32_t word0 = registers_[kRegFetchFirst], word1 = registers_[kRegFetchFirst + 1];
      const uint32_t word2 = registers_[kRegFetchFirst + 2], word3 = registers_[kRegFetchFirst + 3];
      uint32_t swizzle[4];
      for (uint32_t i = 0; i < 4; ++i) swizzle[i] = (word3 >> (1 + 3 * i)) & 7;
      uint32_t id = 0;
      if (presenter_.SurfaceTexture((word1 >> 12) << 12, swizzle, &id, (word0 >> 2) & 0xFF, (word2 & 0x1FFF) + 1,
                                    ((word2 >> 13) & 0x1FFF) + 1, word1 & 0x3F)) {
        texture.id = id;
        texture.valid = true;
        if (interface_surfaces_.insert((word1 >> 12) << 12).second) {
          REXLOG_INFO("[render] interface draws surface {:08X}, {}x{}", (word1 >> 12) << 12, (word2 & 0x1FFF) + 1,
                      ((word2 >> 13) & 0x1FFF) + 1);
        }
      } else if (!FindTexture(&texture, kRegFetchFirst, false, true)) {
        ++ui_.no_texture;
        return;
      }
    }
    const uint32_t triangles = count / 3;
    const uint32_t output = primitive == 8 ? triangles * 6 : triangles * 3;
    uint32_t first = 0;
    UiVertex* out = presenter_.Reserve(output, &first);
    if (!out) return;
    for (uint32_t t = 0; t < triangles; ++t) {
      UiVertex v[3];
      for (uint32_t i = 0; i < 3; ++i) {
        DecodeVertex(*layout, vertex_data, vertex_endian, vertex_index(t * 3 + i), vertex_words, v[i]);
        if (textured) {
          v[i].u *= texture.u_scale;
          v[i].v *= texture.v_scale;
        }
      }
      *out++ = v[0];
      *out++ = v[1];
      *out++ = v[2];
      if (primitive == 8) {  // rectangle list: the fourth corner completes the parallelogram
        UiVertex corner = v[0];
        const float* a = &v[0].x;
        const float* b = &v[1].x;
        const float* c = &v[2].x;
        float* d = &corner.x;
        for (size_t i = 0; i < sizeof(UiVertex) / sizeof(float); ++i) d[i] = a[i] + c[i] - b[i];
        *out++ = v[2];
        *out++ = v[0];
        *out++ = corner;
      }
    }
    // Button icons (32x32 textures) are drawn whole (sims3_049) but show only their left half: a later interface
    // draw covers the rest. Each 32x32 icon's square is kept for the frame, and later triangles of the same frame
    // that overlap one are logged (a few frames' worth), to name what covers it.
    if (cover_lines_ < 120) {
      const UiVertex* drawn = out - output;
      const auto pixel_x = [&](const UiVertex& c) { return (c.x + 1.0f) * kWidth / 2.0f; };
      const auto pixel_y = [&](const UiVertex& c) { return (1.0f - c.y) * kTargetHeight / 2.0f; };
      if (census_.total_swaps != icon_frame_) {
        icon_frame_ = census_.total_swaps;
        icon_rects_.clear();
      }
      const uint32_t word0 = registers_[kRegFetchFirst], word1 = registers_[kRegFetchFirst + 1];
      const uint32_t word2 = registers_[kRegFetchFirst + 2];
      const uint32_t width = (word2 & 0x1FFF) + 1, height = ((word2 >> 13) & 0x1FFF) + 1;
      if (textured && width == 32 && height == 32 && output >= 6) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (uint32_t i = 0; i < output; ++i) {
          x0 = std::min(x0, pixel_x(drawn[i]));
          x1 = std::max(x1, pixel_x(drawn[i]));
          y0 = std::min(y0, pixel_y(drawn[i]));
          y1 = std::max(y1, pixel_y(drawn[i]));
        }
        for (const auto& r : icon_rects_) {  // a second 32x32 draw over an earlier icon
          if (cover_lines_ >= 120 || x1 <= r[0] || x0 >= r[2] || y1 <= r[1] || y0 >= r[3]) continue;
          if (!cover_seen_.insert((uint64_t(uint32_t(r[4])) << 32) ^ (word1 >> 12) ^ 0x5151).second) continue;
          ++cover_lines_;
          REXLOG_INFO("[ui] icon {:08X} at {:.0f},{:.0f}-{:.0f},{:.0f} covered by 32x32 {:08X} at {:.0f},{:.0f}-{:.0f},{:.0f} "
                      "mode {} blend {:08X}", uint32_t(r[4]), r[0], r[1], r[2], r[3], (word1 >> 12) << 12, x0, y0, x1, y1,
                      mode, Register(0x2201));
        }
        if (icon_rects_.size() < 16) icon_rects_.push_back({x0 + 2.0f, y0 + 2.0f, x1 - 2.0f, y1 - 2.0f, float((word1 >> 12) << 12)});
        if (icon_state_logs_ < 30 && icon_states_.insert((uint64_t(word1 >> 12) << 32) ^ depth_control ^ Register(0x2081) ^
                                                        (uint64_t(Register(0x2082)) << 16) ^ Register(kRegRbStencilRefMask)).second) {
          ++icon_state_logs_;
          REXLOG_INFO("[ui] icon {:08X} own state: rect {:.0f},{:.0f}-{:.0f},{:.0f}, depth {:08X}, stencil {:08X}, scissor "
                      "{:08X} {:08X}, blend {:08X}, mask {:X}, colourctl {:08X}, mode {}",
                      (word1 >> 12) << 12, x0, y0, x1, y1, depth_control, Register(kRegRbStencilRefMask),
                      Register(0x2081), Register(0x2082), Register(0x2201), write_mask, Register(0x2202), mode);
        }
      } else if (!icon_rects_.empty()) {
        for (uint32_t t = 0; t + 2 < output && cover_lines_ < 120; t += 3) {
          const UiVertex* tri = drawn + t;
          float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
          for (uint32_t i = 0; i < 3; ++i) {
            x0 = std::min(x0, pixel_x(tri[i]));
            x1 = std::max(x1, pixel_x(tri[i]));
            y0 = std::min(y0, pixel_y(tri[i]));
            y1 = std::max(y1, pixel_y(tri[i]));
          }
          for (const auto& r : icon_rects_) {
            if (x1 <= r[0] || x0 >= r[2] || y1 <= r[1] || y0 >= r[3]) continue;
            // Once per icon, covering texture, shader and triangle: the HUD repeats every frame, the menus do not.
            const uint64_t seen_key = (uint64_t(uint32_t(r[4])) << 32) ^ (uint64_t(word1 >> 12) << 8) ^ capture_.pixel_shader ^
                                      (uint64_t(t / 3) << 56) ^ uint64_t(mode);
            if (!cover_seen_.insert(seen_key).second) continue;
            ++cover_lines_;
            std::string corners;
            for (uint32_t i = 0; i < 3; ++i) {
              corners += fmt::format(" ({:.1f},{:.1f} uv {:.3f},{:.3f} c0 {:.2f} {:.2f} {:.2f} {:.2f} mix {:.2f} a {:.2f})",
                                     pixel_x(tri[i]), pixel_y(tri[i]), tri[i].u, tri[i].v, tri[i].c0[0], tri[i].c0[1],
                                     tri[i].c0[2], tri[i].c0[3], tri[i].c1[2], tri[i].c1[3]);
            }
            float c2[4], c3[4], solid[4];
            Constant4(true, 2, c2);
            Constant4(true, 3, c3);
            Constant4(true, 0, solid);
            REXLOG_INFO("[ui] frame {} icon {:08X} at {:.0f},{:.0f}-{:.0f},{:.0f} covered by vs {:016x} mode {} triangle {} "
                        "of {}: texture {:08X} {}x{} format {} tiled {} (textured {}), blend {:08X} alpha {:X} ref {}, mask {:X}, "
                        "depth {:08X}, stencil {:08X}, c2 {} {} {} {}, c3 {} {} {} {}, solid {} {} {} {};{}",
                        icon_frame_, uint32_t(r[4]), r[0], r[1], r[2], r[3], capture_.vertex_shader, mode, t / 3,
                        output / 3, (word1 >> 12) << 12, width, height, word1 & 0x3F, word0 >> 31, textured,
                        Register(0x2201), Register(0x2202) & 15, std::bit_cast<float>(Register(0x210E)), write_mask,
                        depth_control, Register(kRegRbStencilRefMask), c2[0], c2[1], c2[2],
                        c2[3], c3[0], c3[1], c3[2], c3[3], solid[0], solid[1], solid[2], solid[3], corners);
            break;
          }
        }
      }
    }
    UiUniforms uniforms{};
    Constant4(true, 2, uniforms.c2);
    Constant4(true, 3, uniforms.c3);
    Constant4(true, 0, uniforms.solid);
    uniforms.mode[0] = mode;
    const uint32_t top_left = Register(0x2081), bottom_right = Register(0x2082);
    const uint32_t left = top_left & 0x7FFF, top = (top_left >> 16) & 0x7FFF;
    const uint32_t right = std::min(bottom_right & 0x7FFF, kWidth);
    const uint32_t bottom = std::min((bottom_right >> 16) & 0x7FFF, kHeight);
    if (left >= right || top >= bottom) return;
    if (false && probe_.state == 0 && textured && !presenter_.icon_root().empty()) {  // half-icon probe, off
      const uint32_t word2 = registers_[kRegFetchFirst + 2];
      const UiVertex* drawn = out - output;
      float x0 = 1e9f, y0 = 1e9f;
      for (uint32_t i = 0; i < output; ++i) {
        x0 = std::min(x0, (drawn[i].x + 1.0f) * kWidth / 2.0f);
        y0 = std::min(y0, (1.0f - drawn[i].y) * kTargetHeight / 2.0f);
      }
      if ((word2 & 0x1FFF) == 31 && ((word2 >> 13) & 0x1FFF) == 31 && x0 > 50.0f && x0 < 75.0f && y0 > 545.0f &&
          y0 < 570.0f && ++probe_.sightings == 400) {
        probe_.state = 1;
        probe_.target = presenter_.bound_colour();
        probe_.file = std::fopen((presenter_.icon_root() + "/probe.bin").c_str(), "wb");
        REXLOG_INFO("[probe] B icon {:08X} at {:.1f},{:.1f}, frame {}, target {}", (registers_[kRegFetchFirst + 1] >> 12) << 12,
                    x0, y0, census_.total_swaps, probe_.target ? "bound" : "none");
        ProbeSnapshot("before the icon");
        std::vector<uint8_t> icon;
        if (presenter_.ReadIcon(texture.id, &icon)) {
          if (FILE* f = std::fopen((presenter_.icon_root() + "/probe_icon.bin").c_str(), "wb")) {
            std::fwrite(icon.data(), 1, icon.size(), f);
            std::fclose(f);
          }
          REXLOG_INFO("[probe] icon texture read back at draw time");
        }
        SetEdramTarget();
      }
    }
    presenter_.SetBlendConstants(&registers_[0x2105]);
    presenter_.Draw(uniforms, texture.id, textured, first, output, Register(0x2201), write_mask,
                    DkScissor{left, top, right - left, bottom - top}, depth_control,
                    Register(kRegRbStencilRefMask), Register(0x2202) & 15, std::bit_cast<float>(Register(0x210E)));
    skip_ = DrawSkip::kDrawn;
    ++ui_.drawn;
  }

  // An EDRAM surface is identified by its base and its width in samples: pitch 640 at 4x and pitch 1280
  // at 1x are the same memory, and Direct3D clears through the first to reach the second.
  uint32_t TargetKey(uint32_t info) const {
    const uint32_t surface = Register(kRegRbSurfaceInfo);
    const uint32_t pitch = (surface & 0x3FFF) * (((surface >> 16) & 3) == 2 ? 2 : 1);
    return ((info & 0xFFF) << 16) | std::min(pitch, kTargetWidth);
  }

  // 2 for a 4x MSAA draw into EDRAM whose last other use was a 1x target, else 1. Every other draw records its
  // sample count, so a 2x scene (whose 4x clears follow another rule) keeps today's mapping.
  float MsaaViewScale() {
    const uint32_t surface = Register(kRegRbSurfaceInfo);
    const uint32_t msaa = (surface >> 16) & 3;
    const bool depth_only = (Register(kRegRbModeControl) & 7) == 5;
    const uint32_t key = TargetKey(depth_only ? Register(kRegRbDepthInfo) : Register(kRegRbColorInfo));
    if (msaa != 2) {
      target_msaa_[key] = msaa;
      return 1.0f;
    }
    const auto it = target_msaa_.find(key);
    return it != target_msaa_.end() && it->second == 0 ? 2.0f : 1.0f;
  }

  void SetEdramTarget() { presenter_.SetTarget(TargetKey(Register(kRegRbColorInfo)), TargetKey(Register(kRegRbDepthInfo))); }

  void WindowOffset(int32_t* x, int32_t* y) const {
    *x = *y = 0;
    if (!(Register(0x2205) & 0x10000)) return;
    const uint32_t offset = Register(0x2080);
    *x = int32_t(offset << 17) >> 17;
    *y = int32_t((offset >> 16) << 17) >> 17;
  }

  // Three vertices of a rectangle list, for the draws that are not really drawing.
  bool ReadRectangle(const VertexLayout& layout, uint32_t stride, float* x0, float* y0, float* x1, float* y1,
                     float* z = nullptr, float* colour = nullptr, uint32_t rectangle = 0) {
    const uint32_t fetch = kRegFetchFirst + layout.slot * 2;
    const uint32_t address = registers_[fetch] & 0x1FFFFFFC, endian = registers_[fetch + 1] & 3;
    const uint32_t words = (registers_[fetch + 1] >> 2) & 0xFFFFFF;
    const uint32_t first = rectangle * 3 * stride;
    if (words < first + stride * 3 || !PhysicalRangeMapped(address, (first + stride * 3) * 4)) return false;
    const uint8_t* data = memory_->TranslatePhysical(address) + first * 4;
    const auto real = [&](uint32_t i) {
      uint32_t value;
      std::memcpy(&value, data + i * 4, 4);
      return std::bit_cast<float>(GpuSwap(value, endian));
    };
    *x0 = std::min({real(0), real(stride), real(stride * 2)});
    *y0 = std::min({real(1), real(stride + 1), real(stride * 2 + 1)});
    *x1 = std::max({real(0), real(stride), real(stride * 2)});
    *y1 = std::max({real(1), real(stride + 1), real(stride * 2 + 1)});
    if (z) *z = real(2);
    if (colour) {
      for (uint32_t i = 0; i < 4; ++i) colour[i] = real(3 + i);
    }
    return true;
  }

  // Direct3D clears with a rectangle over the whole surface, often at another sample count than the
  // surface is used at, so this is a host clear rather than a draw.
  bool ClearDraw(uint32_t initiator) {
    if ((initiator & 0x3F) != 8 || capture_.vertex_shader != 0x539ea9e08aa83f0cull) return false;
    const VertexLayout* layout = FindVertexLayout(capture_.vertex_shader);
    float x0, y0, x1, y1, z, colour[4];
    if (!layout || !ReadRectangle(*layout, 7, &x0, &y0, &x1, &y1, &z, colour)) return false;
    ProfileScope scope(kProfClear);
    const uint32_t pitch = Register(kRegRbSurfaceInfo) & 0x3FFF;
    BeginFrame();
    SetEdramTarget();
    const uint32_t control = Register(kRegRbDepthControl), word = Register(kRegRbStencilRefMask);
    const uint32_t mask = (Register(kRegRbModeControl) & 7) == 4 ? Register(kRegRbColorMask) & 0xF : 0;
    const bool clear_depth = (control & 4) != 0;
    const uint32_t stencil_mask = (control & 1) ? (word >> 16) & 255 : 0;
    // A surface can be a little narrower than its pitch: the shadow map is 1024 wide at pitch 1040.
    const bool whole = x0 <= 0.0f && y0 <= 0.0f && x1 + float(pitch / 8) >= float(pitch);
    if (whole) {
      presenter_.Clear(mask, colour, clear_depth, z, stencil_mask, word & 255);
      return true;
    }
    const float scale = MsaaViewScale();
    // Part of a surface, often several rectangles in one draw, in window coordinates (no viewport transform).
    // A 4x view of a 1x target addresses 2x2 samples per pixel. The headshot's white background (one 1x strip
    // and one 4x block) and the shadow map's depth are cleared this way; draws of rectangle lists are not
    // otherwise supported, so these were lost.
    int32_t window_x, window_y;
    WindowOffset(&window_x, &window_y);
    const uint32_t rectangles = (initiator >> 16) / 3;
    for (uint32_t i = 0; i < rectangles; ++i) {
      if (i && !ReadRectangle(*layout, 7, &x0, &y0, &x1, &y1, &z, colour, i)) break;
      const auto edge = [&](float v, int32_t offset, uint32_t limit) {
        return uint32_t(std::clamp(int32_t(std::lround((v + float(offset)) * scale)), 0, int32_t(limit)));
      };
      const uint32_t left = edge(x0, window_x, kTargetWidth), right = edge(x1, window_x, kTargetWidth);
      const uint32_t top = edge(y0, window_y, kTargetHeight), bottom = edge(y1, window_y, kTargetHeight);
      if (left >= right || top >= bottom) continue;
      const DkScissor area{left, top, right - left, bottom - top};
      presenter_.Clear(mask, colour, clear_depth, z, stencil_mask, word & 255, &area);
    }
    if (!partial_clear_logged_) {
      partial_clear_logged_ = true;
      REXLOG_INFO("[render] partial clear: {} rectangles, last {}-{} x {}-{}, pitch {}, scale {}", rectangles, x0, x1,
                  y0, y1, pitch, scale);
    }
    return true;
  }

  void ResolveDraw() {
    ProfileScope scope(kProfResolve);
    const VertexLayout* layout = FindVertexLayout(capture_.vertex_shader);
    float x0, y0, x1, y1;
    if (!layout || !ReadRectangle(*layout, 2, &x0, &y0, &x1, &y1)) {
      ++ui_.unknown;
      return;
    }
    const uint32_t control = Register(0x2318), dest = Register(0x2319) & 0x1FFFFFFF, pitch = Register(0x231A);
    const bool depth = (control & 7) == 4;
    int32_t offset_x, offset_y;
    WindowOffset(&offset_x, &offset_y);
    const int32_t left = std::max(int32_t(x0), 0), top = std::max(int32_t(y0), 0);
    const int32_t source_x = std::max(left + offset_x, 0), source_y = std::max(top + offset_y, 0);
    const uint32_t width = uint32_t(std::max(int32_t(x1) - left, 0)), height = uint32_t(std::max(int32_t(y1) - top, 0));
    BeginFrame();
    const uint32_t key = TargetKey(depth ? Register(kRegRbDepthInfo) : Register(kRegRbColorInfo));
    const bool swap = !depth && ((Register(0x231B) >> 24) & 1);
    const uint32_t atlas_width = pitch & 0x3FFF, atlas_height = (pitch >> 16) & 0x3FFF;
    if (!depth && !capture_.active && capture_.frames_done < 2 && atlas_height >= 256 &&
        (atlas_width == 256 || atlas_width == 512 || atlas_width == 1024) && AtlasMode() &&
        !presenter_.HasSurface(dest)) {
      StartCapture();
    }
    const uint32_t copy_info = Register(0x231B) & 0xFFFFFF;
    if (resolve_formats_.insert(copy_info).second) {
      REXLOG_INFO("[render] resolve to {:08X}: {}x{}, endian {}, format {}, number {}, exponent {}", dest, pitch & 0x3FFF,
                  (pitch >> 16) & 0x3FFF, copy_info & 7, (copy_info >> 7) & 63, (copy_info >> 13) & 7,
                  (copy_info >> 16) & 63);
    }
    // Black shade: the ambient cube's 32x32 faces resolve black. The draws before each tiny resolve, and why each
    // was or was not drawn, show what fills them.
    // Green portraits: the draws before every distinct small resolve (headshots, outfit thumbnails), once each.
    if (!depth && (pitch & 0x3FFF) <= 640 && recent_dumps_ < 150 &&
        recent_keys_.insert((uint64_t(dest) << 32) ^ pitch).second) {
      ++recent_dumps_;
      std::string list;
      for (const auto& line : recent_draws_) list += " | " + line;
      REXLOG_INFO("[render] before resolve {:08X} {}x{} (key {:08X}):{}", dest, pitch & 0x3FFF, (pitch >> 16) & 0x3FFF, key,
                  list);
    }
    recent_draws_.clear();
    presenter_.Resolve(key, depth, swap, dest, pitch & 0x3FFF, (pitch >> 16) & 0x3FFF, uint32_t(source_x), uint32_t(source_y),
                       uint32_t(left), uint32_t(top), std::min(width, kTargetWidth), std::min(height, kTargetHeight),
                       (copy_info & 7) == 2 && ((copy_info >> 7) & 63) == 6 && ((copy_info >> 13) & 7) == 0);
    if (presenter_.TakeWroteBack()) texture_memo_.clear();
    ++census_.host_resolves;
    if (control & 0x300) {
      SetEdramTarget();
      const uint32_t packed = Register(0x231E);
      const float colour[4] = {float((packed >> 16) & 255) / 255.0f, float((packed >> 8) & 255) / 255.0f,
                               float(packed & 255) / 255.0f, float(packed >> 24) / 255.0f};
      const uint32_t clear_depth = Register(0x231D);
      presenter_.Clear((control & 0x100) ? 0xF : 0, colour, (control & 0x200) != 0,
                       float(clear_depth >> 8) / 16777215.0f, (control & 0x200) ? 0xFF : 0, clear_depth & 255);
    }
    skip_ = DrawSkip::kDrawn;
  }

  void NoteUntranslated() {
    skip_ = DrawSkip::kShader;
    ++ui_.unknown;
    const uint64_t pair = capture_.vertex_shader ^ (capture_.pixel_shader * 31);
    if (ui_.unknown_pairs.insert(pair).second) {
      REXLOG_WARN("[render] no translation yet: vs {:016x} ps {:016x}", capture_.vertex_shader, capture_.pixel_shader);
    }
  }

  // Copies one vertex buffer to host byte order. Several draws of a frame share a buffer.
  bool VertexBuffer(uint32_t slot, DkBufExtents* out) {
    const uint32_t fetch = kRegFetchFirst + slot * 2;
    const uint32_t address = registers_[fetch] & 0x1FFFFFFC, endian = registers_[fetch + 1] & 3;
    const uint32_t words = (registers_[fetch + 1] >> 2) & 0xFFFFFF;
    if ((registers_[fetch] & 3) != 3 || !words || !PhysicalRangeMapped(address, words * 4)) return false;
    const uint64_t key = (uint64_t(address) << 32) | (words << 2) | endian;
    if (const auto found = frame_buffers_.find(key); found != frame_buffers_.end()) {
      *out = found->second;
      return true;
    }
    ProfileScope scope(kProfVertex);
    const size_t bytes = size_t(words) * 4;
    const auto* in = reinterpret_cast<const uint32_t*>(memory_->TranslatePhysical(address));
    const auto convert = [&](uint8_t* to) {
      auto* output = reinterpret_cast<uint32_t*>(to);
      if (endian == 2) {
        for (uint32_t i = 0; i < words; ++i) output[i] = std::byteswap(in[i]);
      } else {
        for (uint32_t i = 0; i < words; ++i) output[i] = GpuSwap(in[i], endian);
      }
    };
    // Most vertex buffers never change, so one seen in an earlier frame is kept converted across frames and only
    // hashed to catch changes; one unchanged for 60 checks is hashed every 4th frame (always after a write-back).
    // A buffer seen for the first time takes the per-frame copy, so the cache holds only reused ones.
    const uint64_t frame = presenter_.frame();
    const uint32_t generation = presenter_.vertex_cache_generation(), epoch = presenter_.write_epoch();
    VertexCacheEntry& entry = vertex_cache_[key];
    if (kKeepVertices && entry.generation == generation && entry.gpu) {
      const bool rest = entry.stable >= 60 && entry.epoch == epoch && ((frame + (address >> 6)) & 3) != 0;
      if (rest) {
        ++g_vcache[0];
      } else {
        entry.epoch = epoch;
        const uint64_t hash = HashMemory(reinterpret_cast<const uint8_t*>(in), bytes, false);
        if (hash == entry.hash) {
          ++entry.stable;
          ++g_vcache[1];
        } else {
          convert(entry.cpu);
          presenter_.FlushVertexCache(entry.cpu, bytes);
          entry.hash = hash;
          entry.stable = 0;
          ++g_vcache[2];
          g_vertex_bytes += bytes;
        }
      }
      *out = DkBufExtents{entry.gpu, uint32_t(bytes)};
      frame_buffers_[key] = *out;
      return true;
    }
    if (kKeepVertices && entry.seen && entry.seen != frame && bytes >= 256) {
      const Deko3dPresenter::Arena kept = presenter_.AllocateVertexCache(bytes);
      if (kept.cpu) {
        convert(kept.cpu);
        presenter_.FlushVertexCache(kept.cpu, bytes);
        entry.cpu = kept.cpu;
        entry.gpu = kept.gpu;
        entry.generation = generation;
        entry.hash = HashMemory(reinterpret_cast<const uint8_t*>(in), bytes, false);
        entry.stable = 0;
        entry.epoch = epoch;
        g_vertex_bytes += bytes;
        *out = DkBufExtents{entry.gpu, uint32_t(bytes)};
        frame_buffers_[key] = *out;
        return true;
      }
    }
    entry.seen = frame;
    entry.gpu = 0;
    g_vertex_bytes += bytes;
    Deko3dPresenter::Arena block = presenter_.Allocate(bytes, 16);
    if (!block.cpu) return false;
    convert(block.cpu);
    *out = DkBufExtents{block.gpu, uint32_t(bytes)};
    frame_buffers_[key] = *out;
    if (vertex_cache_.size() > 200000) vertex_cache_.clear();  // keys that never repeat; entry is not used after this
    return true;
  }

  // A bank's copy holds the constants some earlier shader read (covered). It is reused when it covers every
  // constant this shader reads (used, from the build's shader table) and none of those was written since.
  // Otherwise a new 4 KB block gets only the used constants: most shaders read a few low ones and c254-255,
  // and the rest of the block is never read.
  bool ConstantBank(uint32_t bank, uint32_t first, const uint64_t* used, DkGpuAddr* out) {
    ConstantCopy& copy = constant_bank_[bank];
    bool reuse = copy.valid;
    if (!kConstantMasks) reuse = false;
    for (uint32_t i = 0; i < 4 && reuse; ++i) reuse = (used[i] & (~copy.covered[i] | copy.dirty[i])) == 0;
    if (reuse) {
      *out = copy.gpu;
      return true;
    }
    Deko3dPresenter::Arena block = presenter_.Allocate(4096, 256);
    if (!block.cpu) return false;
    const uint32_t* from = &registers_[first];
    if (!kConstantMasks || (used[0] & used[1] & used[2] & used[3]) == ~0ull) {
      std::memcpy(block.cpu, from, 4096);
    } else {
      for (uint32_t i = 0; i < 4; ++i) {
        for (uint64_t bits = used[i]; bits; bits &= bits - 1) {
          const uint32_t vec = i * 64 + uint32_t(std::countr_zero(bits));
          std::memcpy(block.cpu + vec * 16, from + vec * 4, 16);
        }
      }
    }
    for (uint32_t i = 0; i < 4; ++i) {
      copy.covered[i] = used[i];
      copy.dirty[i] = 0;
    }
    copy.gpu = block.gpu;
    copy.valid = true;
    *out = block.gpu;
    return true;
  }

  DkResHandle TextureHandle(uint32_t index, bool cube) {
    const uint32_t first = kRegFetchFirst + index * 6;
    const uint32_t word0 = registers_[first], word1 = registers_[first + 1], word3 = registers_[first + 3];
    const uint32_t sampler = ((word0 >> 10) & 7) == 0 ? 1 : 0;
    const uint32_t fetch_mode = ((word0 >> 2) & 0xFF) | ((word3 & 1) << 8) | (((word3 >> 13) & 0x3F) << 9) |
                                ((word1 & 0x3F) << 16);
    if ((fetch_mode & 0xFFFF) && fetch_modes_.insert(fetch_mode).second) {
      REXLOG_INFO("[render] texture mode: format {}, signs {:02X}, integer {}, exponent {}", fetch_mode >> 16,
                  fetch_mode & 0xFF, (fetch_mode >> 8) & 1, (fetch_mode >> 9) & 0x3F);
    }
    uint32_t id = 0;
    if (cube) {
      // The black stand-in stays for anything that is not a real cube fetch.
      Deko3dPresenter::Texture texture;
      const bool real = kRealCubeMaps && ((registers_[first + 5] >> 9) & 3) == 3 && (word0 & 3) == 2;
      id = (real && FindTexture(&texture, first, true)) ? texture.id : presenter_.cube_descriptor();
    } else {
      uint32_t swizzle[4];
      for (uint32_t i = 0; i < 4; ++i) swizzle[i] = (word3 >> (1 + 3 * i)) & 7;
      const uint32_t word2 = registers_[first + 2];
      if (!presenter_.SurfaceTexture((word1 >> 12) << 12, swizzle, &id, (word0 >> 2) & 0xFF, (word2 & 0x1FFF) + 1,
                                     ((word2 >> 13) & 0x1FFF) + 1, word1 & 0x3F)) {
        // Big-endian 32-bit data is what the GPU's own resolves leave: with no surface it reads as noise.
        if ((word1 & 0x3F) == 6 && ((word1 >> 6) & 3) == 2 && (word2 & 0x1FFF) >= 31 &&
            missing_surfaces_.size() < 60 && missing_surfaces_.insert((word1 >> 12) << 12).second) {
          REXLOG_INFO("[render] resolved texture {:08X}, {}x{}, has no surface; {}", (word1 >> 12) << 12,
                      (word2 & 0x1FFF) + 1, ((word2 >> 13) & 0x1FFF) + 1, presenter_.Describe((word1 >> 12) << 12));
          // Test runs only: a Sim atlas the game copied with the CPU is drawn in the world; capture the next frame.
          if ((word2 & 0x1FFF) + 1 == 1024 && AtlasMode() && world_captures_ < 2) world_capture_next_ = true;
        }
        Deko3dPresenter::Texture texture;
        if (!FindTexture(&texture, first)) {
          ++ui_.no_texture;
          texture = presenter_.WhiteTexture();
        }
        id = texture.id;
      }
    }
    return dkMakeTextureHandle(id, sampler);
  }

  void RenderGeneric(uint32_t initiator, uint32_t dma_base, uint32_t dma_size, uint32_t write_mask,
                     uint32_t depth_control) {
    Deko3dPresenter::LoadedShader* vertex = nullptr;
    Deko3dPresenter::LoadedShader* pixel = nullptr;
    BeginFrame();
    vertex = presenter_.FindShader(capture_.vertex_shader);
    pixel = presenter_.FindShader(capture_.pixel_shader);
    if (!vertex || !pixel) {
      NoteUntranslated();
      return;
    }
    // After the tiled pass Direct3D reloads the 1x EDRAM from the two resolves. This shader draws depth
    // bytes (texture 1) through colour target 0, which shares the depth target's EDRAM, and the scene
    // (texture 0) through colour target 1, which is the main colour surface.
    if (capture_.pixel_shader == 0xb2aa6b1e53b9fe77ull) {
      if ((Register(kRegRbModeControl) & 7) == 4) {
        const uint32_t colour_base = (registers_[kRegFetchFirst + 1] >> 12) << 12;
        const uint32_t depth_base = (registers_[kRegFetchFirst + 6 + 1] >> 12) << 12;
        const uint32_t second = Register(0x2003);
        if (presenter_.RestoreDepth(depth_base, TargetKey(Register(kRegRbColorInfo)))) ++census_.depth_restores;
        if (presenter_.RestoreColour(colour_base, TargetKey(second))) ++census_.colour_restores;
        if (!restore_logged_) {
          restore_logged_ = true;
          REXLOG_INFO("[render] EDRAM reload: colour {:08X} to RT1 {:08X}, depth {:08X} to RT0 {:08X}", colour_base,
                      second, depth_base, Register(kRegRbColorInfo));
        }
      }
      skip_ = DrawSkip::kDrawn;
      return;
    }
    const uint32_t primitive = initiator & 0x3F, source = (initiator >> 6) & 3, count = initiator >> 16;
    Deko3dPresenter::GenericDraw d;
    switch (primitive) {
      // The ambient cube is built from a list of 6144 points, one per texel (sims3_066): without them it stayed
      // black and every shaded surface lost its ambient light.
      case 1: d.primitive = DkPrimitive_Points; break;
      case 3: d.primitive = DkPrimitive_LineStrip; break;
      case 4: d.primitive = DkPrimitive_Triangles; break;
      case 5: d.primitive = DkPrimitive_TriangleFan; break;
      case 6: d.primitive = DkPrimitive_TriangleStrip; break;
      case 13: d.primitive = DkPrimitive_Quads; break;
      default: ++ui_.unknown; return;
    }
    if (source == 1 || !count) {
      ++ui_.unknown;
      return;
    }
    d.vertex = &vertex->shader;
    d.pixel = &pixel->shader;
    d.vertex_hash = capture_.vertex_shader;
    d.pixel_hash = capture_.pixel_shader;

    // Constants: only those each shader reads (whole banks for shaders with relative addressing).
    {
      ProfileScope scope(kProfConstants);
      if (!ConstantBank(0, 0x4000, vertex->info->constants, &d.vertex_constants) ||
          !ConstantBank(1, 0x4400, pixel->info->constants, &d.pixel_constants)) {
        return;
      }
    }
    // Green portraits: the pixel constants of the Sim composite passes and the headshot's body and hair shaders.
    {
      const uint64_t ps = capture_.pixel_shader;
      if ((ps == 0x2c16e2f2bf5b4fc7ull || ps == 0xea6bb4b72519ed06ull || ps == 0x6b842774c4350d2cull ||
           ps == 0x046164ee2c3f234aull || ps == 0xd51f42014baba4acull) && constant_logs_ < 80) {
        static constexpr uint32_t kVecs[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 252, 253, 254, 255};
        const uint32_t count_logged = ps == 0x2c16e2f2bf5b4fc7ull || ps == 0xd51f42014baba4acull ? 15 : 2;
        std::string text;
        uint64_t hash = ps;
        for (uint32_t k = 0; k < count_logged; ++k) {
          const uint32_t* v = &registers_[0x4400 + kVecs[k] * 4];
          text += fmt::format(" c{}=({:.3f},{:.3f},{:.3f},{:.3f})", kVecs[k], std::bit_cast<float>(v[0]),
                              std::bit_cast<float>(v[1]), std::bit_cast<float>(v[2]), std::bit_cast<float>(v[3]));
          for (int i = 0; i < 4; ++i) hash = hash * 0x100000001B3ull ^ v[i];
        }
        if (constant_hashes_.insert(hash).second) {
          ++constant_logs_;
          REXLOG_INFO("[render] constants ps {:016x} mask {:X} swap {}:{}", ps, write_mask, census_.total_swaps, text);
        }
      }
    }
    Deko3dPresenter::Arena constants = presenter_.Allocate(256, 256);
    if (!constants.cpu) return;
    d.state = constants.gpu;

    const uint32_t vte = Register(0x2206);
    const auto real = [&](uint32_t r) { return std::bit_cast<float>(Register(r)); };
    int32_t window_x, window_y;
    WindowOffset(&window_x, &window_y);
    const float scale_x = (vte & 1) ? real(0x210F) : 1.0f, offset_x = (vte & 2) ? real(0x2110) : 0.0f;
    const float scale_y = (vte & 4) ? real(0x2111) : 1.0f, offset_y = (vte & 8) ? real(0x2112) : 0.0f;
    const float scale_z = (vte & 16) ? real(0x2113) : 1.0f, offset_z = (vte & 32) ? real(0x2114) : 0.0f;
    // Direct3D clears part of a 1x target through a 4x MSAA view of the same EDRAM, at half the pitch: there a
    // pixel is 2x2 samples, so its coordinates are half the 1x ones (a 1x clear covers a strip and the 4x one the
    // rest: the headshot's white background, the shadow map's depth). Doubled here, for 1x targets only.
    const float sample_scale = MsaaViewScale();
    // A headshot's second copy: 1x at pitch 640, viewport centred at x 384.
    if (presenter_.PortraitDumps() && offset_x == 384.0f && (Register(kRegRbSurfaceInfo) & 0x3FFF) == 640 &&
        ((Register(kRegRbSurfaceInfo) >> 16) & 3) == 0) {
      presenter_.ArmPortrait();
    }
    const float state[8] = {sample_scale * scale_x * 2.0f / float(kTargetWidth),
                            -sample_scale * scale_y * 2.0f / float(kTargetHeight),
                            scale_z,
                            0.0f,
                            sample_scale * (offset_x + float(window_x)) * 2.0f / float(kTargetWidth) - 1.0f,
                            1.0f - sample_scale * (offset_y + float(window_y)) * 2.0f / float(kTargetHeight),
                            offset_z,
                            0.0f};
    const int32_t misc[4] = {(Register(0x2180) & (1u << 18)) ? int32_t((Register(0x2181) >> 8) & 255) : -1,
                             int32_t((vte >> 10) & 1), int32_t(((vte >> 8) & 1) | (((vte >> 9) & 1) << 1)),
                             int32_t(Register(0x2102))};
    std::memcpy(constants.cpu, &registers_[0x4900], 32);
    std::memcpy(constants.cpu + 32, state, sizeof(state));
    std::memcpy(constants.cpu + 64, misc, sizeof(misc));

    d.fetch_count = vertex->info->fetch_count;
    for (uint32_t i = 0; i < d.fetch_count; ++i) {
      if (!VertexBuffer(vertex->info->fetch[i], &d.fetch[i])) {
        ++ui_.unknown;
        return;
      }
    }

    const uint64_t texture_start = armGetSystemTick();
    const uint32_t vertex_textures = vertex->info->textures_2d | vertex->info->textures_cube;
    for (uint32_t i = 0; i < 16 && vertex_textures; ++i) {
      if (vertex_textures >> i & 1) {
        d.vertex_slot[d.vertex_texture_count] = i;
        d.vertex_handle[d.vertex_texture_count++] = TextureHandle(i, (vertex->info->textures_cube >> i) & 1);
      }
    }
    const uint32_t pixel_textures = pixel->info->textures_2d | pixel->info->textures_cube;
    for (uint32_t i = 0; i < 32; ++i) {
      if (pixel_textures >> i & 1) {
        d.pixel_slot[d.pixel_texture_count] = i;
        d.pixel_handle[d.pixel_texture_count++] = TextureHandle(i, (pixel->info->textures_cube >> i) & 1);
      }
    }

    g_profile[kProfTexture] += armGetSystemTick() - texture_start;
    d.count = count;
    const uint64_t index_start = armGetSystemTick();
    if (source == 0) {
      const bool index32 = (initiator >> 11) & 1;
      const uint32_t address = dma_base & 0x1FFFFFFF, endian = (dma_size >> 30) & 3;
      const uint32_t bytes = count * (index32 ? 4 : 2);
      if (!PhysicalRangeMapped(address, bytes)) {
        ++ui_.unknown;
        return;
      }
      Deko3dPresenter::Arena indices = presenter_.Allocate(bytes, 4);
      if (!indices.cpu) return;
      const uint8_t* in = memory_->TranslatePhysical(address);
      if (index32) {
        const auto* from = reinterpret_cast<const uint32_t*>(in);
        auto* to = reinterpret_cast<uint32_t*>(indices.cpu);
        for (uint32_t i = 0; i < count; ++i) to[i] = GpuSwap(from[i], endian);
      } else {
        const auto* from = reinterpret_cast<const uint16_t*>(in);
        auto* to = reinterpret_cast<uint16_t*>(indices.cpu);
        if (endian == 1) {
          for (uint32_t i = 0; i < count; ++i) to[i] = std::byteswap(from[i]);
        } else {
          for (uint32_t i = 0; i < count; ++i) to[i] = from[i];
        }
      }
      d.indexed = true;
      d.index32 = index32;
      d.indices = indices.gpu;
    }
    g_profile[kProfIndex] += armGetSystemTick() - index_start;

    ProfileScope record_scope(kProfRecord);
    SetEdramTarget();
    d.blend = Register(0x2201);
    d.write_mask = (Register(kRegRbModeControl) & 7) == 4 ? write_mask : 0;
    d.depth_control = depth_control;
    d.stencil_word = Register(kRegRbStencilRefMask);
    d.stencil_back = Register(0x210C);
    // Every stencil setup the game uses, once each: depth control, ref/mask words, write mask, shaders.
    if ((depth_control & 1) && stencil_setups_.size() < 96 &&
        stencil_setups_.insert(uint64_t(depth_control) << 32 | d.stencil_word).second) {
      REXLOG_INFO("[render] stencil setup {}: control {:08X}, ref/mask {:08X}, back {:08X}, colour mask {:X}, "
                  "vs {:016x} ps {:016x}, census draw {}", stencil_setups_.size(), depth_control, d.stencil_word,
                  d.stencil_back, d.write_mask, capture_.vertex_shader, capture_.pixel_shader, census_.draws);
    }
    // Decals and terrain layers stay above what lies under them through this. PA_SU_SC_MODE_CNTL enables it;
    // the scale is in 1/16 subpixels and the offset in whole depth range, against 2^-24 steps here.
    d.bias_on = ((Register(0x2205) >> 11) & 1) != 0;
    // Culling only in depth-only passes: shadow maps rely on it (a Sim's shadow pass culls the faces toward the
    // light, so the map holds its far side and its lit side clears the small bias). Colour passes draw both sides.
    d.cull = (Register(kRegRbModeControl) & 7) == 5 ? (Register(0x2205) & 7) : 0;
    d.bias_constant = real(0x2381) * 16777216.0f;
    d.bias_slope = real(0x2380) / 16.0f;
    d.alpha_control = Register(0x2202) & 15;
    d.alpha_ref = real(0x210E);
    {  // PA_SU_POINT_SIZE: half width and height in 12.4 fixed point
      const uint32_t point = Register(0x2280);
      d.point_size = std::max(1.0f, float(point & 0xFFFF) / 8.0f);
    }
    const uint32_t top_left = Register(0x2081), bottom_right = Register(0x2082);
    const int32_t apply_x = (top_left >> 31) ? 0 : window_x, apply_y = (top_left >> 31) ? 0 : window_y;
    const int32_t k = int32_t(sample_scale);
    const int32_t left = std::clamp((int32_t(top_left & 0x7FFF) + apply_x) * k, 0, int32_t(kTargetWidth));
    const int32_t top = std::clamp((int32_t((top_left >> 16) & 0x7FFF) + apply_y) * k, 0, int32_t(kTargetHeight));
    const int32_t right = std::clamp((int32_t(bottom_right & 0x7FFF) + apply_x) * k, 0, int32_t(kTargetWidth));
    const int32_t bottom = std::clamp((int32_t((bottom_right >> 16) & 0x7FFF) + apply_y) * k, 0, int32_t(kTargetHeight));
    if (left >= right || top >= bottom) return;
    d.scissor = DkScissor{uint32_t(left), uint32_t(top), uint32_t(right - left), uint32_t(bottom - top)};
    const uint32_t colour_info = Register(kRegRbColorInfo) >> 16;
    if (colour_infos_.insert(colour_info).second) {
      REXLOG_INFO("[render] colour target format {}, exponent bias {}", colour_info & 0xF, (colour_info >> 4) & 0x3F);
    }
    if (capture_.pixel_shader == 0xb26246da28c88210ull && !post_logged_) {
      post_logged_ = true;
      const auto constant = [&](uint32_t i) { return std::bit_cast<float>(registers_[0x4400 + i]); };
      REXLOG_INFO("[render] tone pass constants: c254 {} {} {} {}, c255 {} {} {} {}", constant(1016), constant(1017),
                  constant(1018), constant(1019), constant(1020), constant(1021), constant(1022), constant(1023));
    }
    presenter_.SetBlendConstants(&registers_[0x2105]);
    presenter_.DrawGeneric(d);
    skip_ = DrawSkip::kDrawn;
    ++ui_.generic;
  }

  struct UiStats {
    uint64_t drawn = 0, unknown = 0, no_texture = 0, generic = 0;
    std::unordered_set<uint64_t> unknown_pairs;
  };

  enum class DrawSkip { kDrawn, kOther, kPredicate, kTarget, kShader };
  std::deque<std::string> recent_draws_;
  uint32_t recent_dumps_ = 0;
  std::unordered_set<uint64_t> recent_keys_;
  std::unordered_set<uint64_t> constant_hashes_;
  uint32_t constant_logs_ = 0;
  void NoteRecentDraw(uint32_t initiator) {
    if (recent_dumps_ >= 150) return;
    const uint32_t w0 = registers_[kRegFetchFirst], w1 = registers_[kRegFetchFirst + 1];
    const uint32_t w2 = registers_[kRegFetchFirst + 2];
    recent_draws_.push_back(fmt::format(
        "vs {:016x} ps {:016x} prim {} src {} n {} skip {} unknown {} mode {} rt {:08X} surf {:08X} depth {:08X} "
        "mask {:X} vp {} t0 {:08X} f{} {}x{} t0w5 {:08X} t1 {:08X} f{} t2 {:08X} f{} blend {:08X}",
        capture_.vertex_shader, capture_.pixel_shader, initiator & 0x3F, (initiator >> 6) & 3, initiator >> 16,
        int(skip_), ui_.unknown, Register(kRegRbModeControl) & 7, Register(kRegRbColorInfo), Register(kRegRbSurfaceInfo),
        Register(kRegRbDepthControl), Register(kRegRbColorMask) & 0xF, std::bit_cast<float>(Register(0x210F)),
        (w1 >> 12) << 12, w1 & 0x3F, (w2 & 0x1FFF) + 1, ((w2 >> 13) & 0x1FFF) + 1, registers_[kRegFetchFirst + 5],
        (registers_[kRegFetchFirst + 7] >> 12) << 12, registers_[kRegFetchFirst + 7] & 0x3F,
        (registers_[kRegFetchFirst + 13] >> 12) << 12, registers_[kRegFetchFirst + 13] & 0x3F, Register(0x2201)));
    if (recent_draws_.size() > 24) recent_draws_.pop_front();
  }

  struct DrawLog {
    std::string frame;
    uint64_t hash = 0xCBF29CE484222325ull, previous_hash = 0;
    size_t bytes = 0;
    uint32_t count = 0, same_frames = 0;
    bool flash = false;
  };

  DrawSkip skip_ = DrawSkip::kDrawn;
  DrawLog draw_log_;
  uint32_t idle_reports_ = 0, thread_dumps_ = 0, dumps_done_ = 0;
  uint64_t next_dump_swap_ = 0;
  bool dump_requested_ = false;
  bool partial_clear_logged_ = false;
  bool restore_logged_ = false;
  std::unordered_set<uint32_t> fetch_modes_, colour_infos_, interface_surfaces_, resolve_formats_, missing_surfaces_;
  std::vector<std::array<float, 5>> icon_rects_;
  uint64_t icon_frame_ = ~0ull;
  uint32_t cover_lines_ = 0;
  std::unordered_set<uint64_t> cover_seen_, icon_states_;
  uint32_t icon_state_logs_ = 0;
  uint32_t atlas_events_seen_ = 0;
  bool guest_memory_set_ = false;
  bool post_logged_ = false;
  std::vector<uint32_t> page_checked_;
  uint32_t page_generation_ = 1;
  struct ConstantCopy {
    uint64_t covered[4] = {};  // constants (vec4) the copy holds
    uint64_t dirty[4] = {};    // constants written since the copy
    DkGpuAddr gpu = 0;
    bool valid = false;
  };
  ConstantCopy constant_bank_[2];
  std::unordered_set<uint64_t> stencil_setups_;

  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  std::vector<uint32_t> registers_;
  bool mmio_registered_ = false;
  bool presenting_ = false;
  Deko3dPresenter presenter_;

  std::atomic<bool> active_{false};
  std::atomic<uint32_t> callback_{0};
  std::atomic<uint32_t> callback_data_{0};
  std::atomic<uint32_t> ring_base_{0};
  std::atomic<uint32_t> ring_words_{0};
  std::atomic<uint32_t> ring_generation_{0};
  std::atomic<uint32_t> write_pointer_{0};
  std::atomic<uint32_t> read_writeback_{0};
  std::atomic<uint32_t> counter_{0};
  std::mutex ring_mutex_;
  std::condition_variable ring_cv_;
  uint64_t swaps_ = 0;
  uint32_t waits_timed_out_ = 0;
  Census census_;
  Capture capture_;
  UiStats ui_;
  std::map<std::array<uint32_t, 5>, Deko3dPresenter::Texture> texture_memo_;
  std::unordered_map<uint64_t, DkBufExtents> frame_buffers_;
  struct KnownShader {
    std::vector<uint8_t> code;
    uint64_t hash = 0;
  };
  std::unordered_map<uint64_t, KnownShader> shader_hashes_;
  struct VertexCacheEntry {
    uint8_t* cpu = nullptr;
    DkGpuAddr gpu = 0;
    uint64_t hash = 0, seen = 0;
    uint32_t generation = 0, stable = 0, epoch = ~0u;
  };
  std::unordered_map<uint64_t, VertexCacheEntry> vertex_cache_;
  uint64_t bin_mask_ = ~0ull, bin_select_ = ~0ull;
  rex::system::object_ref<rex::system::XHostThread> vblank_thread_;
  rex::system::object_ref<rex::system::XHostThread> ring_thread_;
};

}  // namespace

std::unique_ptr<rex::system::IGraphicsSystem> CreateDeko3dGraphicsSystem() {
  return std::make_unique<Deko3dGraphicsSystem>();
}

}  // namespace sims3

// Called in place of a D3D worker job that has no callback (see tools/post_codegen.py).
extern "C" void sims3_NullWorkerJob(uint32_t device, uint32_t worker, const uint32_t* fields,
                                    const uint32_t* stream) {
  static std::atomic<uint32_t> seen{0};
  const uint32_t count = seen.fetch_add(1, std::memory_order_relaxed);
  if (count >= 6) return;
  std::string text;
  for (uint32_t i = 0; i < 14; ++i) text += fmt::format(" {:08X}", fields[i]);
  REXLOG_ERROR("[worker] job {} has no callback: device {:08X}, worker {:08X}, fields{}", count, device, worker,
               text);
  text.clear();
  for (uint32_t i = 0; i < 24; ++i) text += fmt::format(" {:08X}", stream[i]);
  REXLOG_ERROR("[worker] job list:{}", text);
}
