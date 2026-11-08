SMAA 1x (Subpixel Morphological Antialiasing) - the reference implementation by Jorge Jimenez, Jose I.
Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez, MIT licence (LICENSE.txt, also at the top of
each file). From https://github.com/iryoku/smaa (master), taken 2026-11-03, unchanged:
  SMAA.hlsl            the shader (used with SMAA_CUSTOM_SL: the porting macros are the renderer's, nr_d3d.cpp)
  AreaTex.h, SearchTex.h  the precomputed lookup textures (R8G8 160x560, R8 64x16)
smaa_hlsl.inc is SMAA.hlsl as C++ string pieces (each under MSVC's literal limit), made by make_inc.py from it;
re-run that after replacing SMAA.hlsl. The renderer's integration: the native renderer's anti-aliasing setting
(TF_NATIVE_AA=2, "SMAA 1x") - nr_d3d.cpp runSmaa.
