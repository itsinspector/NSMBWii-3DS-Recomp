# New 3DS performance and stability report

Scope: the static recompilation of NSMBW (SMNE01) running on the New 3DS
through `runtime/` (citro3d backend `gfx_c3d.c`). This report covers the
working tree as of 2026-10-30, including the uncommitted GPU transform path
(`vshader_tl.v.pica`, `gfx_draw_raw`).

Every number is tagged:
- **[measured]**: from a log in this repository's runs (Azahar `log.txt`,
  PC `NSMBW_PROFILE` sampling, generated-code statistics);
- **[estimate]**: an engineering estimate with the reasoning given; not measured.

Azahar numbers are not hardware numbers. Azahar fixes its own emulated CPU
speed (the log shows `ConfigureNew3DSCPU` unimplemented), and SVCs are
expensive there. Relative shares are still meaningful; absolute FPS must be
measured on a console.

---

## 1. Architecture assessment

```
game.iso ──► recomp/ (Python)            build/gen/ppc_recomp_*.c   3.48 M lines, 39,007 functions
              analyze.py: functions,
              lift.py:   PPC → C, one C function per guest function

runtime/
  cpu.c         SPR/MSR, psq_l/psq_st, indirect calls (binary search), LC DMA
  memory.c      88 MiB guest RAM, 64 KiB page table g_memmap[65536]
  hle_os.c      OS threads = host threads with a baton (one runs at a time),
                interrupts polled (rt_poll), virtual guest clock
  hw.c / gx.c   MMIO, VI/PI/DSP/AI, GX FIFO parser (BP/CP/XF registers)
  gx_vertex.c   vertex loader, CPU transform/lighting/texgen (fallback)
  gx_texture.c  texture decode + cache (half size above 512 on 3DS)
  gfx_c3d.c     GX → citro3d: TEV → PICA combiners, EFB copies as render
                targets, GPU transform (vshader_tl.v.pica)
  ax_hle.c dsp.c  audio
```

Design decisions that matter for performance:

1. **Guest registers live in memory** (`PPCContext *c`, `c->r[]`, `c->f[]`,
   `c->cr[]`). Every guest instruction reads and writes that struct.
2. **All memory accesses go through `host_ptr()`**, which returns `uint8_t *`
   (`runtime/include/ppc_context.h`). A `char` pointer may alias anything, so
   after every guest store the C compiler must assume `c->r[]` changed and
   reload it. This is the single biggest code-quality problem in the
   generated code (§3, P3).
3. **One GX primitive = one 3DS draw.** `gx_draw_primitive` (gx_vertex.c) ends
   in `gfx_draw_raw` / `gfx_draw_indexed`, each doing the full per-draw setup
   (textures, TEV program, fragment ops, scissor, uniforms, `C3D_DrawElements`).
   The PC OpenGL backend merges draws; the 3DS backend does not.
4. **Everything runs on core 0.** `plat_cpu_count()` returns 1 on 3DS
   (`platform_3ds.c`), so `hw.c` never starts the GX thread: FIFO parsing,
   vertex work and citro3d submission run inline with the game.

## 2. Measured profile

### 2.1 3DS (Azahar, your session of 2026-10-30 19:00–19:30, heavy scene)

From `sdmc:/3ds/nsmbw/log.txt`, `[prof]` and `[tl]` lines (counters per 120
presented frames, divided by 120) **[measured]**:

| Per frame | Value |
|---|---|
| FPS | 0.6 |
| draws sent through the GPU transform | ~7,550 |
| draws falling back to the CPU transform: texture matrix index per vertex | ~4,255 |
| draws falling back to the CPU transform: lit color channel | ~391 |
| **total draws** | **~12,200** |
| CPU time: draw submission | 38–40 % |
| CPU time: vertex transform (the CPU fallbacks) | 25–26 % |
| CPU time: game + everything else | 27–30 % |
| GPU wait | 0–1 % |
| guest clock | 9 % of real time |

Lighter scenes in my runs (boot, title): 3–11 FPS, transform 0–1 %,
submission 5–7 %, game 55–80 %.

### 2.2 PC build, same C code, level 1-1 (sampling profiler, `NSMBW_PROFILE`)

**[measured]**, share of all samples on all threads:

| Share | Function | Meaning |
|---|---|---|
| 28–30 % | `f_801B4EA0` `SelectThread` | guest idle loop spinning for the next interrupt |
| 7–8 % | `f_801A3880` (AX library, near `__AXOutInit`) | guest audio library |
| 11 % | `QueryPerformanceCounter` + `plat_time_us` | host time queries |
| 4–6 % each | `texgen_regular`, `mul34`, `read_comps`, `load_vertex`, `transform` | CPU vertex path |
| 1.6 % | `ppc_call_indirect` | binary search per indirect call |

### 2.3 Generated code statistics (`build/gen`, **[measured]**)

| Construct | Occurrences |
|---|---|
| `c->r[...]` references | 2,198,746 |
| `c->f[...]` / `c->ps1[...]` | 267,395 / 103,848 |
| compares writing a CR field (`ppc_cmp`/`ppc_cmpl`/`ppc_fcmp`/`set_cr0`) | 83,165 |
| CR bit reads | 93,423 |
| guest loads / stores (int) | 245,887 / 222,223 |
| float loads/stores (`ldf32`/`stf32`) | 124,190 |
| `ppc_psq_l` / `ppc_psq_st` (out-of-line) | 11,244 |
| `chk_*` checked accessors | 44,365 |
| `ppc_call_indirect` sites | 20,826 |
| `c->lr = ...` stores (one per call) | 145,374 |

---

## 3. Bottlenecks, ranked by expected impact

### P1. Draw-call explosion on 3DS (**measured**: ~12,200 draws/frame, submission 38–40 %)

- **Where:** `gx_draw_primitive` (`runtime/src/gx_vertex.c`), `gfx_draw_raw`
  and `gfx_draw_indexed` (`runtime/src/gfx_c3d.c`).
- **Why it costs:** every primitive repeats:
  - `setup_textures` (texture cache lookups);
  - `cached_program` (hash of ~90 words);
  - `apply_program`: 6 combiners marked dirty;
  - `apply_fragment_ops`, `set_scissor`;
  - uniform uploads;
  - a `C3D_DrawElements`, which emits a full GPU command sequence.

  At 12k draws the command buffer fills several times per frame, and
  `ensure_space` then flushes and waits on the GPU in the middle of the frame.
  The primitives are mostly tiny: tile quads, model strips.
- **Fix:** batch consecutive primitives while the state is unchanged.
  - `gx.c` already funnels every BP/CP/XF register write through
    `bp`/`cp_write`/`xf_write`; add a state generation counter bumped there
    (and on texture changes and XF matrix-memory writes).
  - In the backend, keep an open batch (vertex range, index range, state
    generation). Append when the generation matches; flush on mismatch, EFB
    copy, present, or buffer full.
  - The GPU transform path complicates this: matrix rows are uniforms. Either
    flush when a referenced matrix row changes, or (better) include the
    position-matrix row index per vertex, which the shader already supports
    (`inidx`). Then draws that only change `pnmtx` still batch.
  - The OpenGL backend (`gfx_gl.c`, draw merging) is the reference.
- **Impact [estimate]:** the PC backend reduces the same scenes to 100–225
  draws/frame **[measured on PC]**. A 20–50× reduction in 3DS draws should
  remove most of the 38–40 % submission share, and with it the mid-frame GPU
  waits. This is the largest single win available.
- **Risk:** low, provided the state key is complete. A missed state bit shows
  up as wrong textures or blending on part of the frame, so it is easy to see.
  Verify with the PC build: compare frame dumps with batching on and off.

### P2. CPU transform fallbacks (**measured**: 25 % in heavy scenes)

- **Where:** `gfx_draw_raw` returns 0 for `texmtx_direct` (4,255 draws/frame)
  and for lit channels (391/frame), so `transform()` / `light_channel()` /
  `texgen_regular()` in `gx_vertex.c` run on the CPU.
- **Fix:**
  - **Per-vertex texture matrices:** the matrix mirror `mtx[66]` in
    `vshader_tl.v.pica` already holds the texture matrix rows (GX keeps
    position and texture matrices in the same memory). Pass each unit's
    texture matrix row as a vertex attribute and address it with `a0.y` /
    a second `mova`, as is already done for positions. PICA has `a0.x`,
    `a0.y` and the loop register; three units need sequential `mova`s.
  - **Lighting:** GX vertex lighting (ambient + diffuse, distance/angle
    attenuation) fits a vertex shader: light parameters as uniforms (each GX
    light is about 4 vec4), loop over at most 8 lights with `loop`. NSMBW
    lights mostly through light textures, so 1–2 hardware lights per lit draw
    is the expected case **[estimate]**; check with a counter first.
- **Impact [estimate]:** removes most of the 25 %. Each fallback draw also
  costs a `transform()` per vertex (~62 ns/vertex on PC **[measured]**, about
  10–20× that on the ARM11 **[estimate]**).
- **Risk:** medium, because the shader has 96 float uniforms and the matrix
  mirror uses 66. Lighting uniforms must fit (8 lights × 4 = 32 is too many
  with everything else). Upload only the enabled lights, or a reduced form
  (colour, position/direction, 2 attenuation vectors), and fall back for
  more than ~4 lights.

### P3. Recompiled code keeps guest state in memory and defeats the optimizer (**measured** structure, impact **[estimate]**)

- **Where:** `recomp/lift.py` (every emitter uses `c->r[n]`, `c->f[n]`,
  `c->cr[n].x`); `ppc_context.h` (`host_ptr` returns `uint8_t *`; `RD`/`WR`
  use `memcpy`).
- **Why it costs on ARM11:**
  1. **Aliasing.** A store through `uint8_t *` (or `memcpy` to it) can alias
     `*c`, so GCC must write back every dirty `c->r[]` before each guest
     store and reload after it. There are ~468k guest loads/stores among 2.2M
     register references: in practice almost every register access is a real
     load/store to the context. The ARM11 has a 16 KiB L1 D-cache and an
     in-order pipeline with a load-use stall.
  2. **CR as 4 separate bytes.** Each of the 83k compares writes 4 bytes
     (`lt`, `gt`, `eq`, `so`), even when the next branch reads one of them.
     The compiler cannot remove dead flag stores because they are memory
     stores into `*c`.
  3. **Every call stores `c->lr`** (145k sites) and goes through `c`. The
     callee reloads every argument from memory.
- **Fix: register caching in the lifter.**
  - Hold the GPRs, FPRs and CR fields a function uses in C local variables
    (`uint32_t r3`, …; CR as one `uint32_t` or separate locals).
  - **Write back** to `c` before every call, indirect call, `rt_poll`,
    `setjmp` site, return, and any runtime helper that reads `c`
    (`ppc_psq_*`, `ppc_mfspr`/`mtspr`, `ppc_lswi`…).
  - **Reload after** each call. Volatile registers must be reloaded; the
    non-volatile ones too, because `_restgpr_*`/`_restfpr_*` helpers and the
    runtime's context switches (`OSLoadContext` via `longjmp`) can change
    them.
  - Locals whose address is never taken cannot alias memory, so GCC keeps
    them in ARM registers and removes dead CR stores on its own.
  - This is the approach of XenonRecomp's "non-volatile registers as locals"
    option and of N64Recomp, which uses locals throughout.
- **Impact [estimate]:**
  - **Leaf functions:** matrix math (`PSMTX*`), collision, the actor update
    code that is not call-heavy, 1.5–3× faster.
  - **Call-heavy code** (C++ virtual dispatch everywhere in NSMBW) gains less,
    because every call forces a flush and reload.
  - The game share is 27–80 % of the frame **[measured]**, so this is the
    main lever once P1/P2 are done.
- **Risk:** the highest of this list.
  - `OSSaveContext` call sites (`returns_twice`): the `setjmp` already lives
    in the caller's frame, so locals must be flushed before it and reloaded
    after.
  - Interrupt dispatch from `rt_poll` reads and writes the whole context.
  - Functions split across `goto`/tail calls.
  - **Mitigation:** an A/B check needs a deterministic mode, see §6.3.

### P4. Single-core execution (**measured**: `plat_cpu_count() == 1`, GX inline)

- **Where:** `plat_cpu_count` (`platform_3ds.c`); `gx_put` in `hw.c` falls
  back to `gx_feed` inline; all `threadCreate(..., -2, ...)` calls run on the
  default core.
- **What the New 3DS supports:**
  - in New 3DS mode (`CpuSpeed: 804MHz`, `SystemModeExt`, already in
    `config/nsmbw3ds.rsf`) applications can create threads on **core 2**,
    which is dedicated to the application;
  - **core 1** is shared with the system: usable up to the limit set with
    `APT_SetAppCpuTimeLimit` (max 80 %), which suits audio;
  - core 3 is reserved for the system.
- **Fix:** run the existing GX ring consumer (`gx_thread` in `hw.c`) on core 2
  (`threadCreate(gx_thread, NULL, stack, prio, 2, false)`), so FIFO parsing,
  vertex decode and citro3d submission overlap with game logic. Conditions:
  - all citro3d calls must stay on that thread: present, EFB copies and the
    texture uploads that `gx_get_texture` triggers;
  - `gx_texture.c` must stay safe (it already defers frees with `retire()`);
  - draw-done/token interrupts already go through `hw.c` state.
- **Impact [estimate]:** in heavy scenes rendering is ~65 % of the frame
  **[measured]** and the game ~30 %. Running them in parallel bounds the frame
  by the larger of the two, up to ~1.5× once P1/P2 shrink rendering. It
  matters more after P3, when the game and the renderer are closer in cost.
- **Risk:** medium (races). The PC build already runs this exact split (GX
  thread + render thread) **[measured: stable at 60 FPS]**, so most
  synchronization is proven; citro3d on a non-main thread is the new part.

### P5. Time queries and per-draw profiling (**measured** on PC: 11 % of samples)

- **Where:**
  - `PROF_BEGIN`/`PROF_END` (`runtime.h`) around every primitive's transform
    and submission (`gx_vertex.c`) and every texture upload (`gfx_c3d.c`);
  - `check_interrupts` and `hw_update` (`hle_os.c`, `hw.c`) call
    `plat_time_us()` / `rt_time_us()` on every poll.
- **Why it costs on 3DS:**
  - `plat_time_us` = `svcGetSystemTick()` (an SVC: a kernel entry, several
    hundred cycles **[estimate]**, very expensive under Azahar);
  - followed by a 64-bit division (`__aeabi_uldivmod`, a software routine
    on ARM11);
  - at 12k draws × 2–4 timestamps, that is tens of thousands of SVCs per
    frame.
- **Fix:**
  - replace the per-draw timing with counters and a few timestamps per frame
    (`[prof]` only needs per-frame totals);
  - convert ticks to µs with a multiply-shift instead of a division;
  - make `PROF_*` a build option;
  - in `check_interrupts`, read the clock only when a deadline could have
    passed (cache the next deadline in ticks).
- **Impact [estimate]:** small on hardware once P1 cuts the draw count. On
  Azahar it is visible: SVCs are slow there and Azahar is where this game is
  tested.
- **Risk:** none for game logic.

### P6. Indirect calls by binary search (**measured**: 1.6 % on PC, 20,826 sites)

- **Where:** `lookup_function` / `ppc_call_indirect` (`runtime/src/cpu.c`):
  a binary search over 39,007 entries (≈16 dependent loads, scattered over
  ~300 KiB) on every virtual call or function pointer, and on every call to a
  target that is not a known function start.
- **Fix:** a direct-mapped cache in front of the search (e.g. 4096 entries
  indexed by `(addr >> 2) & 4095`, 32 KiB). Or emit a perfect-hash or
  sorted-by-hotness table from the recompiler.
- **Impact [estimate]:** a few % of game time. NSMBW calls virtual
  `execute`/`draw` on every actor every frame.
- **Risk:** none.

### P7. Paired-single loads/stores out of line (**measured**: 11,244 sites; hot in nw4r math)

- **Where:** `ppc_psq_l` / `ppc_psq_st` (`runtime/src/cpu.c`). Each call
  reads `c->gqr[n]`, decodes the type/scale, calls `ps_load_one` twice and
  writes `c->f`/`c->ps1`. `PSMTXConcat` (`f_801C0540`) alone does 12 of them.
- **Fix:** in `lift.py`, emit an inline fast path for the common case,
  float type with scale 0 (`if (c->gqr[n] == 0) { f = ldf32(ea); ps1 = ldf32(ea+4); } else ppc_psq_l(...)`).
  Better still, HLE the hottest SDK math routines (`PSMTXConcat`,
  `PSMTXMultVec`, `PSVECNormalize`, …) as native C, as `hle_*` overrides
  already do for OS functions. They are pure functions of memory.
- **Impact [estimate]:** significant for the scene graph (`CalcWorld`,
  `CalcView` call `PSMTXConcat` per node); a few % overall.
- **Risk:** low; GQR values are architectural state and easy to check.

### P8. Checked memory accessors in whole functions (**measured**: 44,365 sites)

- **Where:** `Lifter.acc()` in `recomp/lift.py`. When a function computes any
  hardware address (`hw_func`), every access in it becomes `chk_*`, a compare
  and branch per access, including stack accesses (`r1`) and small-data
  accesses (`r2`/`r13`).
- **Fix:** keep plain accessors for `r1`, `r2` and `r13` bases: they can never
  point into `0xCC000000`.
- **Impact [estimate]:** small (1–2 % of game time).
- **Risk:** none.

### P9. Single-precision math done in double

- **Where:** paired-single and `fadds`-type ops are emitted as `double` math
  followed by a `(float)` round (example in `f_801C0540`).
- **Why it might matter on ARM11:** VFP11 double multiply/add have lower
  throughput than single, and double divide/sqrt take about twice as long
  **[estimate from the ARM11 VFP11 timings; not measured here]**.
- **Fix (careful):** for `ps_add/sub/mul/div` and `fadds/fsubs/fmuls/fdivs`
  whose inputs are known single-rounded (results of other single ops or of
  `lfs`/`psq_l`), compute in `float`. For these operations, rounding the
  exact result to double and then to single gives the same value as rounding
  straight to single, so the result is identical. `ps_madd`/`fmadds` are NOT
  equivalent (fused vs. unfused) and must stay in double.
- **Impact:** unknown. Measure first: run a microbenchmark of the generated
  `PSMTXConcat` on hardware in both forms.
- **Risk:** medium (float semantics). Do it last.

### P10. Memory and textures (the "crunch the textures" idea)

- **Measured:** texture work is 0–1 % of CPU time and GPU wait is 0–6 %, so
  texture size is not a frame-rate bottleneck today.
- **Where it does matter:** linear memory (`GPU_TEX_BUDGET` 14 MiB in
  `gfx_c3d.c`) and VRAM. All textures are uploaded as `GPU_RGBA8`
  (4 bytes/texel). Converting at upload to `RGB565` (opaque), `RGBA5551`
  (1-bit alpha) or `RGBA4` would halve memory and texture bandwidth, and PICA
  supports all three.
- **Recommendation:** do it after P1–P4, when the GPU may become the
  limiter; then measure with `C3D_GetDrawingTime`. The real memory
  constraint is the Old 3DS (not a target, because guest RAM alone is
  88 MiB).

### P11. Guest idle spin and thread switches

- **Measured on PC:** `SelectThread` spins for 30 % of samples.
- **On 3DS:** idle is 5–20 %, which costs nothing while everything is on one
  core. With P4 it would steal time only from core 0, which then has nothing
  else to do, so it is not urgent.
- **Thread switches:** guest thread switches go through host semaphores
  (`hle_os.c`, a baton). Count them per frame before changing anything; each
  is an SVC pair on 3DS.

---

## 4. Crash diagnosis

**Evidence available [measured]:**
- your last Azahar session ran 31 minutes and ended normally;
- there is no `FATAL` line and no `[stack]` peak warning in `log.txt`;
- Azahar's own log shows no exception.

So the crashes cannot be attributed from the current logs. The candidates
below are ranked by how likely they are given the code; each comes with a way
to confirm it.

1. **Host stack overflow in guest threads.**
   - `HOST_STACK` is 512 KiB on 3DS (`hle_os.c`) and the main thread has
     1 MiB (`__stacksize__`). Recompiled C frames are larger than PPC frames,
     and the call depth equals the guest's (nw4r scene-graph recursion, plus
     interrupt handlers nested on top through `rt_poll`).
   - `threadCreate` stacks have **no guard page**, so an overflow silently
     corrupts the heap and crashes later somewhere unrelated.
   - *Confirm:* put a canary word at the bottom of each host stack and check
     it in `check_interrupts`; log the deepest `note_stack_depth` per thread
     at exit.
2. **Command buffer and linear/VRAM exhaustion from 12k draws per frame.**
   - `ensure_space` flushes when the buffer is ¾ full, but a single EFB copy
     or present sequence emitted right at the limit can still overflow;
     citro3d then asserts (`svcBreak`).
   - EFB-copy render targets are created per destination address/size and
     only freed through the LRU (`gtex_alloc`), so VRAM fragments over long
     sessions.
   - *Confirm:* log `vramSpaceFree()` / `linearSpaceFree()` with `[prof]`;
     P1 removes most of the pressure anyway.
3. **Alignment faults.**
   - ARMv6 handles unaligned `LDR`/`STR`/`LDRH`, but **not** `LDRD`/`STRD`,
     `LDM`/`STM` or VFP loads (alignment fault → data abort).
   - `ld64`/`st64` (`RD`/`WR` via `memcpy`) and `stf32`/`ldf32` on guest
     addresses that are not naturally aligned are the risk. GCC may combine
     adjacent loads into `LDRD`.
   - *Confirm:* disassemble the hot users (`arm-none-eabi-objdump -d
     nsmbw3ds.elf | grep -c ldrd`) and check data-abort addresses in crash
     dumps.
4. **Unimplemented or unknown targets** (`ppc_unimplemented`,
   `ppc_call_indirect` to an unknown address, `ppc_fell_off`). These are
   deterministic and already reported with `rt_fatal` and the guest `lr`, so
   any such crash would appear in `log.txt` as `FATAL`.
5. **Races between host threads on core 0.** The timer thread
   (`timer_thread`, every 1 ms) and the service threads preempt the guest
   thread. Shared state includes `g_rt_pending` and the DSP/AI state in
   `hw.c`. This becomes more relevant with P4.

**Systematic strategy (instead of guessing):**
1. **Crash capture on every thread.** Install an exception handler (libctru
   `threadOnException`, one per created thread) that writes
   `sdmc:/3ds/nsmbw/crash_<n>.txt` with:
   - the fault type, address and the ARM registers;
   - the host PC and LR (resolve offline with
     `arm-none-eabi-addr2line -e nsmbw3ds.elf`);
   - the guest `c->lr`, `c->r[1]`, and the last 64 guest function entries.
2. **A cheap always-on guest trace.** A ring of the last 64 function entries
   (one store per call) is affordable. The trace build's 1024-entry
   `RT_TRACE` is the same idea and already exists.
3. **On hardware**, Luma3DS writes ARM11 exception dumps to `/luma/dumps/arm11`
   on its own. Pair them with the `.elf` of the same build.
4. **Classify crashes** by host PC + guest function, count frequency, and fix
   the top signature first.
5. **Under Azahar**, enable the GDB stub for host backtraces on the
   reproducible ones.

---

## 5. New 3DS specifics: supported vs. theoretical

| Item | Status |
|---|---|
| 804 MHz + 2 MiB L2 | **Supported**, already requested (`CpuSpeed: 804MHz`, `EnableL2Cache: true` in the rsf) and `osSetSpeedupEnable(true)` in `main`. Azahar ignores it (`ConfigureNew3DSCPU` unimplemented in its log). |
| Core 2 for application threads | **Supported** in New 3DS mode. Basis of P4. |
| Core 1 for application work | **Supported, limited** (`APT_SetAppCpuTimeLimit`, max 80 %, shared with system services). Good for audio mixing (`ax_hle.c`). |
| NEON / float SIMD | **Not available.** ARM11 has VFPv2 only; ARMv6 SIMD is integer 8/16-bit and useless for the float paths. |
| Thumb to shrink code | **Not useful.** ARMv6K has only Thumb-1 (no Thumb-2): code would be slower. |
| Instruction cache | **Real concern, unmeasured.** 16 KiB L1 I-cache vs ~15 MiB of generated code; the L2 helps. Grouping hot functions (link order from a profile) is theoretical until measured. |
| PICA vertex shader for T&L | **Supported, done** (`vshader_tl.v.pica`); P2 extends it. |
| PICA fragment lighting | Exists, but it is per-pixel lighting with LUTs, not GX vertex lighting. Not a match; use the vertex shader (P2). |
| GPU timing (`C3D_GetDrawingTime`, `C3D_GetProcessingTime`) | **Supported**, cheap; use it in the profiling plan. |

## 6. Profiling and benchmarking plan

### 6.1 Always-on, near-zero cost

Per 120 frames, logged as now:
- frame-time histogram (bucket the present-to-present interval, one
  timestamp per frame);
- draws, vertices and fallback reasons (the `[tl]` line, already there);
- GPU drawing/processing time (`C3D_GetDrawingTime`);
- `linearSpaceFree`, `vramSpaceFree`, texture cache bytes, command-buffer
  flushes per frame (`ensure_space` mid-frame flushes);
- guest thread switches per frame;
- CPU split by subsystem from **one timestamp per subsystem per frame**,
  not per draw (P5).

### 6.2 Sampling profiler for guest functions

On a thread on core 1:
- wake every 1 ms and read the last entry of the guest function-entry ring
  (§4.2); count hits in a hash table;
- dump the top 40 to the log every N seconds with symbols
  (`g_ppc_symbols`).

The trace build already does this (`take_sample` in `hle_os.c`); the cost is
the per-call store only.

### 6.3 Deterministic A/B benchmark (needed for P3 and P1)

The PC runs are not deterministic: the virtual guest clock adapts to idle
time (`rt_time_adapt`), so two runs reach scenes at different frames
**[measured: present counts differ run to run]**. Add a mode where:
- guest time advances by a fixed amount per guest VI frame;
- scripted input (`NSMBW_INPUT`) drives the run;
- RAM is hashed every 60 VI frames.

Two builds that must behave identically (register caching on/off, batching
on/off) can then be compared by hash, and the first divergent frame
localises a bug.

### 6.4 Fixed benchmark scenes

On hardware and in Azahar:
1. the title demo;
2. 1-1 start;
3. the intro cutscene;
4. a busy level section.

Record the median and 99th-percentile frame time per scene for every stage
below.

## 7. Implementation roadmap

Each stage is independent, testable alone, and benchmarked against the
previous one with §6.4.

| Stage | Change | Files | Pass criterion |
|---|---|---|---|
| S0 | Instrumentation: per-frame timing instead of per-draw (P5), GPU time, memory counters, crash handler + trace ring (§4) | `runtime.h`, `gfx_c3d.c`, `gx_vertex.c`, `hle_os.c`, `platform_3ds.c` | Same FPS or better; a crash writes a usable report |
| S1 | Draw batching (P1) | `gx.c` (state generation), `gfx_c3d.c` | Draws/frame down ≥10×; frame dumps identical on PC with batching on/off |
| S2 | Per-vertex texture matrices + vertex lighting in the shader (P2) | `vshader_tl.v.pica`, `gfx_c3d.c` | `[tl]` CPU fallbacks ≈ 0; T&L share < 2 % |
| S3 | Indirect-call cache (P6), inline `psq_l`/`psq_st` fast path and SDK math HLE (P7), plain accessors for `r1`/`r2`/`r13` (P8) | `cpu.c`, `recomp/lift.py`, HLE table | Bit-identical RAM hashes (§6.3); game share down |
| S4 | GX + citro3d on core 2 (P4) | `hw.c`, `platform_3ds.c`, `gfx_c3d.c` | No new crashes over a 30-minute run; FPS up in heavy scenes |
| S5 | Register caching in the lifter (P3), behind a recompiler flag, enabled per function set | `recomp/lift.py` | Identical RAM hashes over the deterministic run; game share ↓ |
| S6 | Optional: single-precision math (P9), 16-bit texture formats (P10) | `lift.py`, `gfx_c3d.c` | Microbenchmark gain on hardware; no visual change |

Ordering rationale:
- S1 and S2 attack the 64–66 % of the frame that rendering takes in heavy
  scenes **[measured]**, with low risk.
- S3 is cheap and safe.
- S4 only pays off fully once the game and the renderer are of similar
  cost.
- S5 is the biggest remaining CPU win but needs the S0/§6.3 tooling to be done
  safely.

**What to expect, honestly [estimate]:**
- S1 + S2 should take heavy scenes from fully render-bound to game-bound
  (several× FPS in those scenes).
- After that, the frame rate is set by the recompiled game code, and only S5
  (with S4's parallelism) moves it substantially.
- A steady 60 FPS on a New 3DS is not something this plan can promise.
  30 FPS in most scenes is a realistic target to aim and measure for.
