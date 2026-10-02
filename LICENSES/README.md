# Licensing and provenance

The bridge, including its new in-process Wine OpenXR adaptation, is distributed
under LGPL-2.1-or-later. See the root LICENSE. Commercial use is permitted.
Existing source-level notices remain in force: independently reusable legacy
BSL-1.0 files retain that licence and its notice (BSL-1.0.txt). This does not
claim ownership of third-party code or remove their permissions or obligations.

The following files were copied unchanged from ValveSoftware/Proton commit
5b89db940e0ebe3a137a6009a3589232fe084c09, wineopenxr/:

- src/in_process/proton/make_openxr: LGPL-2.1-or-later, with its original notices.
- src/in_process/proton/openxr_private.h and loader_structs.h: the repository's
  LICENSE.proton (copied as Proton-BSD-3-Clause.txt); retain its Valve notice.

Generated files carry the Khronos xr.xml Apache-2.0 OR MIT notice emitted by the
upstream generator. The generated implementation also incorporates generator
code: treat the combined runtime as LGPL-2.1-or-later, rather than assuming that
this generated header notice is the only applicable licence.

Wine headers and import libraries are build dependencies, not relicensed here.
The build consumes them from the separately supplied SDK/source/runtime. DXMT,
OpenXR-SDK and Monado retain their own licences. Distribution of binaries must
include the corresponding LGPL source and the licence/notice requirements of
the components actually bundled.
