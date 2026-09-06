This directory holds patches applied to fetched third-party dependencies (see CMakeLists.txt).

sourcepp-vtx-bounds-check.patch
--------------------------------
Fixes an unhandled, non-catchable STATUS_INVALID_PARAMETER (0xC0000409) crash in
sourcepp's mdlpp VTX parser (src/mdlpp/structs/VTX.cpp). VTX::open() builds
std::span objects from offset/count fields read directly out of the .vtx file
without validating them against the size of the underlying vector
(stripGroup.indices / stripGroup.vertices / stripGroup.topologyIndices). A
malformed or mismatched .vtx (wrong dx-level variant, stale file colliding
inside a VPK, truncated/corrupt data, etc.) can therefore produce an
out-of-range span construction, which trips a fatal MSVC debug-iterator
_STL_VERIFY check and fails the whole process - __try/__except cannot catch
this failure (see src/headers/ModelRenderer.h for details).

This patch adds bounds checks around the three span constructions in
VTX::open() so an out-of-range strip is simply left with an empty span instead
of crashing.

Applied automatically by CMakeLists.txt's sourcepp FetchContent_Declare via
apply-sourcepp-patches.cmake, which skips (re-)applying if the patch is already
present in the fetched source (so re-configuring an existing _deps checkout
does not fail).

If bumping the pinned sourcepp GIT_TAG, re-validate this patch still applies
cleanly against the new upstream VTX.cpp (line-context based diff) - if
upstream fixes this issue independently, delete this patch and the
PATCH_COMMAND entirely.
