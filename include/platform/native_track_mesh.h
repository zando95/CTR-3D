#ifndef NATIVE_TRACK_MESH_H
#define NATIVE_TRACK_MESH_H

#include <macros.h>

// Read-only accessor surface over platform/native_track_mesh.c's resident
// track mesh. Declarations
// are always visible; every accessor is a no-op/zero-returning stub reader
// when CTR_NATIVE_TRACK_RENDERER is off (the .c file's gate simply never
// populates s_trackMesh), so callers don't need their own #ifdef.
//
// Deliberately narrow: this exposes what a draw side (the frame
// recorder, later the 3DS upload/draw path) needs to READ, not the mesh
// builder's internal build-time state (weld table, atlas packer, texture
// intern table). NativeTrackVert itself is exposed -- the 3DS upload
// re-packs it into a citro3d attribute layout and needs the real field
// layout to do that, so a void* here would just move the same knowledge into
// an untyped cast at every call site instead of removing it.

// 16 bytes -- see platform/native_track_mesh.c's struct comment for what each
// field means and where it comes from (LevVertex / TextureLayout / QuadBlock
// draw-order bytes). Positions are model-space, straight from the game's own
// vertex pool.
struct NativeTrackVert
{
	s16 x, y, z;
	s8 drawOrder;
	u8 faceFlags; /* bit0 doubleSided; bits 1-3 faceMode/rotation scratch */
	u8 r, g, b, a;
	u8 u, v;
	u16 texRef;
};

CTR_STATIC_ASSERT(sizeof(struct NativeTrackVert) == 16);

// 1 once a level's resident mesh has been built successfully (the same latch
// NativeTrackMesh_Frame uses internally); 0 before the first successful build
// of a level, or if the gate is compiled out.
int NativeTrackMesh_IsBuilt(void);

// 1 if the CURRENTLY latched mesh was built for this exact mesh_info pointer.
// The frame recorder's own bail-out check (a level transition mid-frame must
// never draw the previous level's resident mesh against the new level's
// visibility) -- narrower than IsBuilt, which only says "something is built".
int NativeTrackMesh_IsBuiltFor(const struct mesh_info *mesh);

// Number of resident per-leaf index ranges in the latched mesh (0 if
// unbuilt).
unsigned NativeTrackMesh_LeafCount(void);

// Leaf i's BSP index (the same index space as mesh->bspRoot / visMem1->
// bspList[0], i.e. a visibility-lookup key) plus its index-buffer range.
// Returns 0 and leaves the outputs untouched when i is out of range or
// nothing is built; 1 on success. Callers walk i = 0..LeafCount()-1, they
// never need to search -- see NativeTrackMesh_LeafIndexForBsp for the
// reverse direction.
int NativeTrackMesh_LeafAt(unsigned i, unsigned *outBspIndex, unsigned *outFirstIndex, unsigned *outIndexCount);
// Atlas unit this range samples; >= the unit count means untextured.
//
// wantPass2 picks WHICH placement for a blended range (see LeafBlend), because
// such a range is drawn twice: 0 gives the primary placement, holding its PASS1
// (opaque body) view for the opaque pass; 1 gives the second placement, holding
// its PASS2 (translucent texels) view for the blend pass. Opaque ranges return
// the same unit either way, so callers that never blend can pass 0 and ignore
// this entirely.
unsigned NativeTrackMesh_LeafAtlasBin(unsigned i, int wantPass2);

// Semi-transparency. 3 means opaque -- the normal
// case and the value returned for an out-of-range i. 0/1/2 are the PSX ABR blend
// modes (B/2+F/2, B+F, B-F) and mark a range the BLEND pass owns: it must be
// drawn after all opaque content, with blending on and depth writes off.
//
// Ranges are homogeneous in this by construction: build-time binning keys
// blended content per atlas RECT, and a rect's ABR comes from its tpage, which
// is part of the rect's own identity key.
unsigned NativeTrackMesh_LeafBlend(unsigned i);

// Texel offset from the rect's primary placement to its PASS2 placement, which
// the blend pass adds to each vertex's baked UV so one set of resident vertices
// can address both decoded views. (0, 0) for opaque ranges and out-of-range i.
void NativeTrackMesh_LeafUvOffset(unsigned i, int *outDx, int *outDy);

// Leaf i's STATIC BSP bounding box (model-space corners, copied from
// mesh->bspRoot[bspIndex].box at BUILD time -- see native_track_mesh.c's
// struct NativeTrackLeafRange comment). Exists so the draw
// side (platform/native_track_frame.c) can compute a leaf's view-z range for
// the depth compose WITHOUT dereferencing a raw mesh_info/BSP pointer on the
// worker thread -- this flat copy is validated/latched by the exact same
// build-or-nothing contract every other accessor here already relies on.
// Returns 0 and leaves the outputs untouched when i is out of range or
// nothing is built; 1 on success.
int NativeTrackMesh_LeafBox(unsigned i, s16 outMin[3], s16 outMax[3]);

// Leaf i's signed per-face drawOrder range (min/max over every face the
// build-time emit loop actually wrote into this leaf -- native_track_mesh.c's
// struct NativeTrackLeafRange comment), copied at BUILD time exactly like
// LeafBox above. The draw side's per-leaf OT-bucket
// depth range used to come from the leaf's bounding box ALONE, which does
// not account for the per-face signed drawOrder byte each vertex's own
// otIndex adds in the shader (ctr_track.v.pica) -- a face whose drawOrder
// pushed it outside the box-derived range would extrapolate the shader's
// lerp past a valid ordinal and get clipped away by the GPU (geometry
// deletion, not mis-layering). Widening the CPU-side range by the leaf's own
// [drawOrderMin, drawOrderMax] before it reaches
// NativeTrackDraw_ComputeLeafDepthRange (platform/native_track_frame.c)
// closes that: every vertex the leaf can ever emit now falls inside the
// range its draw uniform describes. Returns 0 and leaves the outputs
// untouched when i is out of range or nothing is built; 1 on success.
int NativeTrackMesh_LeafDrawOrderRange(unsigned i, s8 *outMin, s8 *outMax);

// Reverse lookup: BSP index -> leaf-range index (into LeafAt), or -1 if that
// BSP index has no resident range (excluded leaf -- water/animated-vertex,
// design 4.4 -- or simply not a leaf). Built once per mesh build, alongside
// the mesh itself, specifically so the per-frame visibility walk (the
// frame recorder) is O(visible leaves) rather than O(leaves x ranges): a
// frame's visible-leaf list comes from the game's own render-list traversal
// as BSP indices, and this is how those get turned into leaf-range indices
// without a linear search per frame.
int NativeTrackMesh_LeafIndexForBsp(unsigned bspIndex);

// Advance every animated track texture (turbo-pad arrows, waterfall
// sheets, lava) to the frame the game has cycled to, rewriting the affected
// resident vertices' UVs in the GPU-resident buffer. Without this the resident
// renderer shows whichever frame happened to be active when the level loaded,
// for the whole race.
//
// MUST be called on the thread that WALKS, after that walk's
// Platform_BeginScene -- platform/native_libgpu.c calls it from
// CtrGpu_ExecDrawOTag. The buffer it writes is the same single linearAlloc
// block the GPU reads, and the previous frame's command list keeps reading it
// until BeginScene has waited for that frame's GPU work; a finished CPU walk is
// not a finished GPU frame. The render queue being FIFO with one worker is what
// keeps the next walk from starting meanwhile. Calling it from the game thread
// instead would need
// either a fence (which costs the render thread's whole win) or a second copy
// of the vertex block.
//
// No-op when nothing is built, when the level has no animated textures, or
// when the gate is compiled out.
void NativeTrackAnim_PatchFrame(void);

// 3DS only. The animated-vertex patch above rewrites only the vertices of the
// leaf slots the frame's snapshot draws. The track draw calls this with the
// snapshot it is about to draw, on the walk, before it issues any leaf: any
// slot in it that the patch has not covered for that snapshot is patched now,
// so no slot is ever drawn with a vertex block older than its own frame.
struct NativeTrackFrameSnapshot;
void NativeTrackAnim_EnsureLeavesPatched(const struct NativeTrackFrameSnapshot *snap);

// Raw vertex/index arrays backing every leaf range above, and their counts.
// NULL/0 when nothing is built. u16 indices -- see the .c file's build-time
// guard against exceeding that format's range.
const struct NativeTrackVert *NativeTrackMesh_Verts(void);
unsigned NativeTrackMesh_VertCount(void);
const u16 *NativeTrackMesh_Indices(void);
unsigned NativeTrackMesh_IndexCount(void);

#endif
