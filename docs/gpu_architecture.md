# OpenFPGA 3D Accelerator Architecture

## 1. Overview

A fixed-function 3D rasterizer built around a **triangle pipeline with span fast-path**. The CPU handles all 3D transforms and submits screen-space geometry. The GPU rasterizes, textures, lights, depth-tests, and writes pixels to the framebuffer — all while the CPU runs game logic in parallel.

```
    CPU (VexRiscv @ 100 MHz)
         |
    [SDRAM Command Ring Buffer]      ← CPU writes commands here
         |
    ┌────┴─────────────────────────────────────────────────┐
    │  GPU (~100 MHz, 5K ALMs, 64 M10K, 6 DSP)            │
    │                                                       │
    │  [DMA Fetch] → [Cmd Decode] → [State Registers]      │
    │                      |                                │
    │              ┌───────┴───────┐                        │
    │              │               │                        │
    │       [Triangle Setup]  [Span Bypass]                 │
    │       (edge equations)  (direct params)               │
    │              │               │                        │
    │              └───────┬───────┘                        │
    │                      │                                │
    │              [Scanline Rasterizer]                     │
    │              (walk edges, emit fragments)              │
    │                      │                                │
    │              [Fragment Processor]                      │
    │              ├─ Texture Cache (M10K)                   │
    │              ├─ Colormap BRAM (M10K)                   │
    │              ├─ Color interpolation                    │
    │              └─ Alpha test                             │
    │                      │                                │
    │              [Depth Test] ←→ SRAM Z-buffer             │
    │                      │                                │
    │              [FB Writer] → AXI4 → SDRAM framebuffer    │
    └───────────────────────────────────────────────────────┘
```

## 2. Resource Budget

| Component               | ALMs      | M10K | DSP | Notes                                    |
|--------------------------|-----------|------|-----|------------------------------------------|
| DMA command fetch        | 300       | 4    | 0   | AXI4 read, ring pointer management       |
| Command decoder + state  | 300       | 2    | 0   | Unpack commands, sticky state registers   |
| Triangle setup           | 1000      | 4    | 4   | Edge equations, attribute gradients (sequential DSP) |
| Scanline rasterizer      | 700       | 4    | 0   | Edge walking, fragment emission           |
| Span bypass path         | 150       | 0    | 0   | Mux span params into rasterizer          |
| Fragment processor       | 1000      | 0    | 2   | Texcoord step, color interp, alpha test  |
| Texture cache            | 300       | 20   | 0   | 4-way set-assoc, 16B lines, AXI4 fill   |
| Colormap BRAM            | 100       | 16   | 0   | 16KB: 64 light × 256 palette             |
| Z-buffer interface       | 400       | 2    | 0   | SRAM read/compare/write pipeline          |
| Framebuffer writer       | 350       | 4    | 0   | 4-byte accumulator, AXI4 burst writes    |
| Misc (FIFO, fence, perf) | 300       | 4    | 0   | Fence counter, stat counters             |
| **Total**                | **4900**  | **60** | **6** |                                      |
| **Budget**               | **5000**  | **64** | **~59** |                                    |

Headroom: ~100 ALMs, 4 M10K.

### 2.1. Expansion Options (if more ALMs available)

| Feature                   | Extra ALMs | Extra M10K | What it enables                     |
|---------------------------|-----------|------------|--------------------------------------|
| Perspective correction    | +400      | +1         | Per-16px subdivision (Quake quality) |
| Bilinear texture filter   | +300      | +2         | Smoother textures (SM64, Wipeout)    |
| Turbulence warp           | +150      | +1         | Quake water/lava/slime effects       |
| 4-column batch mode       | +300      | 0          | Duke3D vlineasm4 optimization        |
| Translucency (FB readback)| +400      | 0          | Duke3D translucent walls             |
| Edge walker coprocessor   | +700      | +2         | HW triangle-to-span decomposition    |

## 3. Command Ring Buffer

### 3.1. Memory Layout

```
SDRAM:  [ ... app data ... | Command Ring (64KB) | ... ]
                             ^                 ^
                             |                 |
                          rd_ptr (GPU)      wr_ptr (CPU)
```

- 64KB default ring buffer in SDRAM
- GPU has a DMA engine that reads commands from the ring
- CPU writes commands sequentially, updates `wr_ptr` register on `kick()`
- GPU advances `rd_ptr` as it completes commands
- Fence commands write a sequence number to a status register the CPU can poll

### 3.2. Command Packet Format

All commands are 32-bit word aligned. First word encodes type + size.

```
Word 0:  [31:24] cmd_type  [23:0] payload_words
Word 1+: command-specific payload
```

| Cmd Type | Name              | Payload Words | Description                        |
|----------|-------------------|---------------|------------------------------------|
| 0x01     | NOP               | 0             | Padding / alignment                |
| 0x02     | FENCE             | 1             | Write fence_value to status reg    |
| 0x10     | CLEAR             | 2             | flags, color, depth                |
| 0x20     | SET_TEXTURE       | 4             | addr, w, h, format, wrap           |
| 0x21     | SET_DEPTH_FUNC    | 1             | depth function enum                |
| 0x22     | SET_BLEND         | 1             | blend mode + alpha ref             |
| 0x23     | SET_FRAMEBUFFER   | 2             | addr, stride                       |
| 0x24     | SET_ZBUFFER       | 2             | addr, stride                       |
| 0x25     | SET_SHADE_MODE    | 1             | gouraud enable                     |
| 0x30     | DRAW_TRIANGLES    | 1 + N×6       | count, then count vertices (6 words each) |
| 0x31     | DRAW_INDEXED      | 2 + V×6 + I   | vert_count, idx_count, verts, indices |
| 0x40     | DRAW_SPAN         | 16            | full span descriptor               |
| 0x41     | DRAW_SPANS_BATCH  | 1 + N×16      | count, then count span descriptors |

### 3.3. Vertex Encoding in Ring (6 words = 24 bytes)

```
Word 0: [31:16] x (12.4)    [15:0] y (12.4)
Word 1: [31:16] z (16-bit)  [15:0] pad
Word 2: s (16.16 fixed-point)
Word 3: t (16.16 fixed-point)
Word 4: w (16.16 fixed-point, 1/W for perspective, 0x10000 for affine)
Word 5: [31:24] a  [23:16] b  [15:8] g  [7:0] r
```

## 4. Triangle Pipeline

### 4.1. Triangle Setup (~15-25 cycles per triangle)

Computes edge equations and attribute gradients using a shared DSP multiplier (sequential, 4 DSP blocks time-shared).

**Edge equations** (3 edges):
```
E(x,y) = A*x + B*y + C
where A = y1 - y0, B = x0 - x1, C = x1*y0 - x0*y1
```

**Attribute gradients** (Z, S, T, W, R/light):
```
dAttr/dx = ((A1*dY20) - (A2*dY10)) / (dX10*dY20 - dX20*dY10)
dAttr/dy = ((A2*dX10) - (A1*dX20)) / (dX10*dY20 - dX20*dY10)
```

The reciprocal `1/det` is computed once using a CLZ + reciprocal LUT + DSP correction multiply (same approach as PocketQuake's perspective unit, ~8-10 cycles).

Setup is amortized: at 320x200, a typical triangle covers 50-500 pixels, making 20-cycle setup negligible.

### 4.2. Scanline Rasterizer (~1 cycle per pixel)

Walks the triangle top-to-bottom, left-to-right:
1. For each scanline Y in the triangle's bounding box:
   - Compute left/right X bounds from edge equations
   - For each pixel X in [left, right]:
     - Emit fragment with interpolated Z, S, T, W, color

The rasterizer uses integer edge stepping (add `A` per X step, add `B` per Y step) — no multiplies in the inner loop.

**Sub-pixel precision**: 12.4 coordinates give 1/16 pixel accuracy. Fill rules (top-left) prevent pixel gaps between adjacent triangles.

### 4.3. Span Bypass

When a DRAW_SPAN command arrives, it skips triangle setup entirely. The span's pre-computed parameters (start coord, step, count) feed directly into the fragment processor. The rasterizer just counts pixels using the span's `count` field.

Two texture addressing modes (selected by span flags):
- **Multiply mode** (Quake): `addr = base + (t >> 16) * width + (s >> 16)`
- **Shift mode** (Duke3D/Doom): `addr = base + ((t >> shift) << bits) | (s >> (32-bits))`

### 4.4 Fragment Processor (~2-4 cycles per pixel)

Per fragment:
1. **Texture address**: compute from S, T (or S/W, T/W with perspective divide)
2. **Texture fetch**: read texel from texture cache
3. **Color combine**:
   - I8 + colormap: `output = colormap[light * 256 + texel]`
   - I8 no colormap: `output = texel`
   - RGB565 + gouraud: `output = texel * vertex_color` (DSP multiply)
4. **Alpha test**: discard if alpha < ref
5. **Depth test**: compare fragment Z against SRAM Z-buffer
6. **Framebuffer write**: accumulate bytes, burst-write to SDRAM

### 4.5. Texture Cache

- **Capacity**: 20 M10K = 20KB
- **Organization**: 4-way set-associative, 64 sets, 16-byte lines (16 texels for I8, 8 for RGB565)
- **Hit latency**: 1 cycle (M10K registered read)
- **Miss latency**: ~10-40 cycles (AXI4 SDRAM burst fill)
- **Fill**: non-blocking prefetch on sequential access patterns

### 4.6. Colormap BRAM

- **Capacity**: 16 M10K = 16KB
- **Layout**: 64 rows × 256 columns = 16384 entries
- **Access**: `colormap[light_level * 256 + texel_index]` → 8-bit output color
- **Upload**: DMA from SDRAM at init time via `of_gpu_colormap_upload()`
- **Shared by**: Doom (colormap.lmp), Duke3D (palookup), Quake (colormap), Descent (lighting tables)

### 4.7. Depth Buffer (SRAM)

- **Format**: 16-bit unsigned per pixel
- **Storage**: External SRAM (256KB available, 320×200×2 = 128KB needed)
- **Interface**: dedicated SRAM port, read-compare-write pipeline
- **Latency**: 1-2 cycles (SRAM is fast)
- **Functions**: less, less-equal, equal, always, none

## 5. Per-Game Mapping

### 5.1. Doom

Doom renders vertical columns (walls) and horizontal spans (floors/ceilings). Pure span engine.

```c
// Wall column
of_gpu_span_t col = {
    .fb_addr  = dest_addr,
    .tex_addr = texture_addr,
    .s        = frac,           // texture V position (16.16)
    .t        = 0,              // unused (1D column)
    .sstep    = fracstep,       // V step per pixel
    .tstep    = 0,
    .count    = column_height,
    .light    = light_level,
    .flags    = OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_COLUMN,
    .fb_stride = 320,           // advance one row per pixel
    .tex_width = tex_height,    // 1D column texture
};
of_gpu_draw_span(&col);
```

### 5.2. Duke3D (Build Engine)

Same span model as Doom but with shift-based texture addressing.

```c
// Floor/ceiling span (hlineasm4)
of_gpu_span_t span = {
    .fb_addr   = dest_addr,
    .tex_addr  = texture_addr,
    .s         = u_fixed,
    .t         = v_fixed,
    .sstep     = u_step,
    .tstep     = v_step,
    .count     = pixel_count,
    .light     = shade,
    .flags     = OF_GPU_SPAN_COLORMAP,
    .fb_stride = 1,
    .tex_shift = shifter,       // Build engine V shift
    .tex_bits  = bits,          // Build engine UV combine width
};
of_gpu_draw_span(&span);
```

### 5.3. Quake

Two options:

**Option A: Span path (like PocketQuake)**
Use `of_gpu_draw_span()` with `OF_GPU_SPAN_PERSP` for perspective-corrected world surfaces, and plain spans for alias models. Closest to existing PocketQuake code — easiest port.

**Option B: Triangle path (like GLQuake)**
Submit BSP surfaces as triangle fans. The GPU handles rasterization and texturing.
```c
of_gpu_bind_texture(&surface_tex);
of_gpu_depth_test(OF_GPU_DEPTH_LESS);
of_gpu_draw_triangles(surface_verts, surface_tri_count * 3);
```

Option B is more general but requires more CPU work for surface cache / lightmap application. Option A is faster for Quake specifically.

### 5.4. Quake 2 / Half-Life

Triangle-based. Quake 2 has a software renderer, but the OpenGL path maps naturally:

```c
// Per surface: base texture × lightmap
of_gpu_bind_texture(&base_texture);
of_gpu_depth_test(OF_GPU_DEPTH_LESS);
// Vertex R = pre-computed light level from lightmap
of_gpu_draw_triangles(surface_verts, tri_count * 3);
```

Multitexture (base × lightmap in one pass) would require a second texture unit — not in the initial 5K ALM budget. Workaround: pre-multiply lightmap into a surface cache (like Quake's software renderer), or do lightmap as a second pass with multiply blend.

### 5.5. Super Mario 64

Triangle-based with vertex colors. Perfect match for the triangle pipeline.

```c
of_gpu_bind_texture(&mario_texture);
of_gpu_depth_test(OF_GPU_DEPTH_LESS);
of_gpu_shade_mode(true);  // Gouraud: vertex RGB modulates texture

of_gpu_vertex_t v[3] = {
    { x0, y0, z0, 0, s0, t0, OF_FP16(1), r0, g0, b0, 255 },
    { x1, y1, z1, 0, s1, t1, OF_FP16(1), r1, g1, b1, 255 },
    { x2, y2, z2, 0, s2, t2, OF_FP16(1), r2, g2, b2, 255 },
};
of_gpu_draw_triangles(v, 3);
```

### 5.6. Descent

Textured polygons with flat/Gouraud shading, 8-bit indexed. Triangle pipeline + colormap.

### 5.7. Wipeout

Textured triangles with Gouraud shading, semi-transparency. Triangle pipeline with vertex colors. Semi-transparent surfaces can use OF_GPU_BLEND_ALPHA or stay in software (like Duke3D translucency).

## 6. Performance Estimates

At 100 MHz, 320×200 (64,000 pixels):

| Configuration               | Cycles/Pixel | Pixels/sec | FPS (fill-limited) |
|-----------------------------|-------------|------------|---------------------|
| Triangle + affine + I8+cmap | ~4-5        | 20-25M     | 312-390             |
| Triangle + affine + RGB565  | ~4-5        | 20-25M     | 312-390             |
| Triangle + perspective + I8 | ~8-12       | 8-12M      | 130-190             |
| Span (Doom/Duke3D style)    | ~3-5        | 20-33M     | 312-520             |
| Span + perspective (Quake)  | ~8-12       | 8-12M      | 130-190             |
| Clear (DMA)                 | ~0.25       | 400M       | N/A                 |

**Fill rate is never the bottleneck at 320×200.** Even the slowest mode (perspective) can fill the screen 2-3× per frame at 30 FPS. The real bottleneck is CPU-side transform, BSP traversal, and game logic — which is exactly why the async model matters.

### 6.1. Overdraw

Typical overdraw ratios:
- Doom: 1.5-2× (no z-buffer, painter's order)
- Duke3D: 1.5-2× (same)
- Quake: 1.2-1.5× (z-buffered, front-to-back BSP)
- SM64: 2-3× (many overlapping polygons)
- Wipeout: 1.5-2×

At 2× overdraw, 128K effective pixels per frame. Still well within fill budget.

## 7. Implementation Phases

### Phase 1: Foundation (span-only, Doom/Duke3D ready)

Build the core infrastructure without triangle setup:
- Command ring buffer + DMA fetch engine
- Command decoder + state registers
- Span rasterizer (stepping engine)
- Texture cache + colormap BRAM
- Framebuffer writer
- Z-buffer interface
- Fence mechanism

**Validates**: async model, texture cache, memory interfaces, basic pixel pipeline.
**Enables**: Doom, Duke3D (full acceleration), Quake (via span path).

### Phase 2: Triangle setup (SM64/Descent/Wipeout ready)

Add the triangle pipeline:
- Edge equation computation (sequential DSP)
- Attribute gradient computation
- Scanline rasterizer (edge walking)
- Feed fragments into the existing fragment processor

**Validates**: triangle rendering, sub-pixel precision, fill rules.
**Enables**: SM64, Descent, Wipeout, Quake 2 / Half-Life (triangle path).

### Phase 3: Quality + features

Based on remaining ALMs after Phase 2:
- Perspective correction (for large triangles / Quake span path)
- Gouraud color interpolation (RGB modulate for SM64, Wipeout)
- Indexed drawing (vertex reuse for meshes)

### Phase 4: Optimization

- Texture prefetch (predict next texel address, start SDRAM fetch early)
- 4-byte FB write batching (already in PocketQuake design)
- Performance counters for profiling
- DMA span list mode (batch SDRAM→GPU for many short spans)
