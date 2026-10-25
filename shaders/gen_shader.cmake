# Compile one HLSL shader to THIS platform's GPU bytecode and wrap it in a C++ header.
#
# Build-time, per-platform shader generation in pure CMake script mode — the build
# needs no Python or other scripting runtime; CMake is already required. Invoked per
# shader by the add_custom_command in the root CMakeLists.txt:
#
#   cmake -DFORMAT=<metallib|spirv|dxil> -DSTAGE=<vert|frag> -DSTEM=<name>
#         -DSRC=<file.hlsl> -DOUT=<header.h> -DTMP=<dir> [-DINCLUDE=<dir>]
#         [-DGLSLANG=<path>] [-DSPIRV_CROSS=<path>] [-DDXC=<path>]
#         -P shaders/gen_shader.cmake
#
# INCLUDE is the engine's shared-header directory (shaders/common), handed to whichever HLSL frontend this
# platform runs so shaders/src/*.hlsl can #include shared kernels. Only glslang and dxc ever see HLSL —
# spirv-cross, `metal` and `metallib` operate on SPIR-V / MSL, where every #include is already resolved.
# Optional: a caller that passes no INCLUDE compiles exactly as before.
#
# Each platform's build compiles the shader format its SDL_GPU backend runs, using
# that platform's native tools — nothing is cross-built and no bytecode is committed:
#
#     macOS   → metallib (HLSL→SPIR-V→MSL, then `xcrun metal`/`metallib` → precompiled) [Metal]
#     Linux   → SPIR-V   (glslang HLSL→SPIR-V)                                           [Vulkan]
#     Windows → DXIL     (dxc HLSL→DXIL)                                                 [Direct3D 12]
#
# The emitted header exposes the exact symbol set the renderer already consumes —
# kSpirv / kDxil / kMetallib plus the three per-format entrypoint constants. Only this
# platform's format is a real byte array; the other two are nullptr constants, which
# selectShader treats as absent at runtime. The renderer's includes and code are
# untouched: the header lands in the build tree under the same include path
# (shaders/generated/<stem>.h) instead of being committed.

cmake_minimum_required(VERSION 3.28)

foreach(_required FORMAT STAGE STEM SRC OUT TMP)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "gen_shader.cmake: -D${_required}=... is required")
    endif()
endforeach()

if(NOT DEFINED GLSLANG)
    set(GLSLANG glslangValidator)
endif()
if(NOT DEFINED SPIRV_CROSS)
    set(SPIRV_CROSS spirv-cross)
endif()
if(NOT DEFINED DXC)
    set(DXC dxc)
endif()

# Include search path for the engine's shared headers, in each frontend's own spelling. Empty when no
# INCLUDE was passed, so the tool invocations below are byte-identical to their pre-include form.
set(_inc_glslang "")
set(_inc_dxc "")
if(DEFINED INCLUDE)
    set(_inc_glslang "-I${INCLUDE}")
    set(_inc_dxc -I "${INCLUDE}")
endif()

file(MAKE_DIRECTORY "${TMP}")
string(REPLACE "." "_" NS "${STEM}")

# BATCHED / GATHER / SPRITE (optional, mutually exclusive): a SECOND compiled variant of a custom shader
# beside the normal one, with a different generated entry point. BATCHED is the instanced-additive region
# variant (the quad's 3 varyings + the replicated gate, returning the delta against a zero source; for
# `// @retropp:additive` shaders). GATHER is the union-shape fullscreen variant that collapses N
# same-stage REPLACE regions into one pass (the shader's own cbuffer rewritten to statics fed from a
# storage buffer's per-region records; for every custom shader EXCEPT additive- or no-gather-declared ones).
# SPRITE inlines the custom body into the sprite fragment (SPRITE_BASE) at its hook marker so the shader runs
# per sprite pixel with sampleSource() reading the sprite's own art — one pass, no scaling with sprite count;
# for every custom shader EXCEPT no-sprite-declared or non-float-param ones. SPRITE_BELOW is the scene-facing
# counterpart: it inlines the body into the BELOW sprite fragment (SPRITE_BELOW_BASE) so the shader distorts /
# grades the composited SCENE beneath the sprite's layer through the silhouette (sampleSource() reads the
# scene, not the art) — one pass per below-custom pipeline, no scaling with sprite count. Each gets a distinct
# symbol (<ns>_batched / <ns>_gather / <ns>_sprite / <ns>_sprite_below) and distinct TMP intermediate names so
# its build never clobbers the normal variant's. Only ever passed for a PREAMBLE (custom) shader.
set(TAG "${STEM}")
if(DEFINED BATCHED)
    string(APPEND NS "_batched")
    set(TAG "${STEM}.batched")
elseif(DEFINED GATHER)
    string(APPEND NS "_gather")
    set(TAG "${STEM}.gather")
elseif(DEFINED SPRITE)
    string(APPEND NS "_sprite")
    set(TAG "${STEM}.sprite")
elseif(DEFINED SPRITE_BELOW)
    string(APPEND NS "_sprite_below")
    set(TAG "${STEM}.sprite_below")
elseif(DEFINED EMISSION_EXTRACT)
    # The emission EXTRACT variant of an emission-declared shader that defines a `float4 emission(float2 uv)`
    # body: a distinct symbol <ns>_emission the engine runs in the extract pass to author the field content.
    # (The MAIN emission variant keeps the plain <ns> symbol — it is the same fullscreen variant, compiled
    # with the emission binding via -DEMISSION, so no suffix.)
    string(APPEND NS "_emission")
    set(TAG "${STEM}.emission")
elseif(DEFINED EMISSION_RECT)
    # The below WRITE variant (<ns>_emission_rect): the same emission() body over the rect-instanced below
    # extract fragment, authoring a Below-scope Custom lens's field over its rect.
    string(APPEND NS "_emission_rect")
    set(TAG "${STEM}.emission_rect")
endif()

# Optional PREAMBLE injection: a game-authored custom shader declares its OWN parameter cbuffer (at
# b1/space3) + main(); the generator prepends the standard preamble (the source texture + sampler +
# sampleSource() + the engine cbuffer at b0; shaders/include/retropp_effect.hlsli) so the shader
# body just samples through sampleSource() and reads its own params. Engine-internal shaders pass no
# PREAMBLE and compile as-is. `_compile_src` is what actually gets compiled below.
#
# The shader's main() is renamed (identifier only — its parameter/return semantics are legal and ignored
# on a non-entry function) and a generated entry point wraps it: on the Viewport evaluation grid (uSnap,
# the crisp default) the wrapper hands the shader the center of the viewport cell its fragment falls in
# and records the fragment's true uv for sampleSource()'s displacement quantization — so an unmodified
# custom shader evaluates exactly like the composeScale = 1 rasterization. On the Output grid the wrapper
# passes the true uv through — smooth output-resolution evaluation, wholesale.
set(_compile_src "${SRC}")
if(DEFINED SPRITE)
    # ── SPRITE-INLINE variant ───────────────────────────────────────────────────────────────────
    # Inject the custom body + its sprite-path plumbing into the sprite fragment (SPRITE_BASE) at the
    # `// @retropp:sprite-custom-hook` marker, so the game shader runs inline per sprite pixel — one pass,
    # no scaling with the sprite count. sampleSource() reads the sprite's own art (SPRITE_PREAMBLE); the
    # shader's cbuffer is rewritten to file-scope statics fed from the sprite-effect record's param texels
    # (register k at texel 2 + k). Params are FLOAT-family only (the record store is a float texture — float
    # values round-trip bit-exact); an int / uint field is a hard error (declare // @retropp:no-sprite).
    file(READ "${SPRITE_PREAMBLE}" _sprite_preamble_text)
    file(READ "${SRC}" _body_text)
    # main → retroppCustomMain (a non-entry function; its parameter / return semantics are ignored).
    string(REGEX REPLACE "float4[ \t\r\n]+main[ \t\r\n]*\\(" "float4 retroppCustomMain("
           _body_text "${_body_text}")
    # Strip // comments before the cbuffer parse (a comment may contain the word "cbuffer").
    string(REGEX REPLACE "//[^\n]*" "" _body_text "${_body_text}")
    # cbuffer field parse — the offset math MIRRORS gen_effect_fields.cmake (same 16-byte register straddle)
    # so the loader's record texels match the C++ packer's bytes, which the renderer uploads verbatim.
    set(_sp_statics "")
    set(_sp_loader "")
    set(_sp_offset 0)
    if(_body_text MATCHES "cbuffer[^{]*{([^}]*)}")
        set(_cb_body "${CMAKE_MATCH_1}")
        string(REPLACE ";" ";;" _cb_body "${_cb_body}")
        string(REGEX MATCHALL "[A-Za-z0-9_]+[ \t\r\n]+[A-Za-z_][A-Za-z0-9_]*" _decls "${_cb_body}")
        foreach(_decl ${_decls})
            string(REGEX MATCH "^([A-Za-z0-9_]+)[ \t\r\n]+([A-Za-z_][A-Za-z0-9_]*)$" _ok "${_decl}")
            if(NOT _ok)
                continue()
            endif()
            set(_htype "${CMAKE_MATCH_1}")
            set(_sname "${CMAKE_MATCH_2}")
            if(_htype STREQUAL "float")
                set(_ssize 4)
                set(_sncomp 1)
            elseif(_htype STREQUAL "float2")
                set(_ssize 8)
                set(_sncomp 2)
            elseif(_htype STREQUAL "float3")
                set(_ssize 12)
                set(_sncomp 3)
            elseif(_htype STREQUAL "float4")
                set(_ssize 16)
                set(_sncomp 4)
            else()
                message(FATAL_ERROR "gen_shader.cmake SPRITE: sprite-path custom params are float-typed "
                                    "(float / float2 / float3 / float4); field '${_sname}' is '${_htype}'. "
                                    "Declare // @retropp:no-sprite to keep this shader off the sprite path.")
            endif()
            math(EXPR _reg_start "${_sp_offset} - (${_sp_offset} % 16)")
            math(EXPR _reg_end "(${_sp_offset} + ${_ssize} - 1) - ((${_sp_offset} + ${_ssize} - 1) % 16)")
            if(NOT _reg_start EQUAL _reg_end)
                math(EXPR _sp_offset "((${_sp_offset} + 15) / 16) * 16")
            endif()
            math(EXPR _sreg "${_sp_offset} / 16")
            math(EXPR _slane "(${_sp_offset} % 16) / 4")
            string(SUBSTRING "xyzw" ${_slane} ${_sncomp} _ssw)
            math(EXPR _stexel "2 + ${_sreg}")     # param register k lives at record texel 2 + k
            string(APPEND _sp_statics "static ${_htype} ${_sname};\n")
            string(APPEND _sp_loader "    ${_sname} = uFxStore.Load(int3(${_stexel}, ri, 0)).${_ssw};\n")
            math(EXPR _sp_offset "${_sp_offset} + ${_ssize}")
        endforeach()
    endif()
    # Remove the shader's own cbuffer block (its params are the statics above).
    string(REGEX REPLACE "cbuffer[^{]*{[^}]*}[ \t\r\n]*;?" "" _body_text "${_body_text}")
    # EMISSION_CONSUMER: an emission-declared stage's Layer-scope sprite variant reads its own blurred
    # silhouette field back through sampleEmission(uv). A Custom step has no emission variant, so the field's
    # content is the art brightpass produced by a synthetic Bloom record (renderer side); the field row + engaged
    # flag ride this record's GATE lanes (texel 1: y = absolute rect-table row, z = engaged), which the seat
    # writes (seatPlacedRegionEmissionFields, custom branch). The read maps the body's within-sprite quad uv to
    # the field's compose position by forward-applying THIS sprite's placement — the adjugate of the screen->quad
    # inverse the fragment carries in inv0..inv2 (the perspective divide cancels the adjugate's scale), then
    # uComposeScale lifts viewport px to the compose px sampleEmissionField reads. So the wrapper takes inv0..inv2
    # and the variant defines RETROPP_SPRITE_CUSTOM_EMISSION (the base call site passes them). sampleEmissionField
    # is already in scope at the hook. A non-consumer sprite variant declares none of this, keeps the
    # three-argument wrapper, and is byte-identical to before.
    set(_sprite_emission_define "")
    set(_sprite_emission_decls "")
    set(_sprite_wrap_params "")
    set(_sprite_emission_wrap "")
    if(DEFINED EMISSION_CONSUMER)
        set(_sprite_emission_define "#define RETROPP_SPRITE_CUSTOM_EMISSION
")
        set(_sprite_emission_decls "static float  retroppEmissionIndex;
static float  retroppEmissionEngaged;
static float3 retroppEmInv0;
static float3 retroppEmInv1;
static float3 retroppEmInv2;
float4 sampleEmission(float2 uv) {
    if (retroppEmissionEngaged == 0.0f) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    float3 a = retroppEmInv0, b = retroppEmInv1, d = retroppEmInv2;
    float3 m0 = float3(b.y * d.z - b.z * d.y, a.z * d.y - a.y * d.z, a.y * b.z - a.z * b.y);
    float3 m1 = float3(b.z * d.x - b.x * d.z, a.x * d.z - a.z * d.x, a.z * b.x - a.x * b.z);
    float3 m2 = float3(b.x * d.y - b.y * d.x, a.y * d.x - a.x * d.y, a.x * b.y - a.y * b.x);
    float3 q  = float3(uv, 1.0f);
    float  w  = dot(m2, q);
    float2 vp = float2(dot(m0, q), dot(m1, q)) / w;
    return sampleEmissionField(vp * uComposeScale, retroppEmissionIndex);
}
")
        set(_sprite_wrap_params ", float3 inv0, float3 inv1, float3 inv2")
        set(_sprite_emission_wrap "    float4 rpGate = uFxStore.Load(int3(1, ri, 0));  // alpha, field row, engaged, _
    retroppEmissionIndex   = rpGate.y;
    retroppEmissionEngaged = rpGate.z;
    retroppEmInv0 = inv0;
    retroppEmInv1 = inv1;
    retroppEmInv2 = inv2;
")
    endif()
    # The injected block: define RETROPP_SPRITE_CUSTOM (skips the base no-op), the emission define (consumers
    # only), the sprite preamble (sampleSource over the art), the emission read (consumers only), the param
    # statics + record-lane loader, the custom body, then the wrapper the sprite loop calls (reads the step's
    # edge off the record head, seats the emission gate for consumers, loads the params, runs the body).
    set(_sprite_inject "
#define RETROPP_SPRITE_CUSTOM
${_sprite_emission_define}${_sprite_preamble_text}
${_sprite_emission_decls}${_sp_statics}void retroppLoadSpriteParams(int ri) {
${_sp_loader}}
${_body_text}
float4 retroppSpriteCustom(float4 c, float2 uv, int ri${_sprite_wrap_params}) {
    float4 rpHead = uFxStore.Load(int3(0, ri, 0));                       // kind, flags, blend, pointCount
    retroppSpriteEdgeStretch = (((uint)rpHead.y & 4u) != 0u) ? 1u : 0u;  // the step's edge (Stretch)
${_sprite_emission_wrap}    retroppLoadSpriteParams(ri);
    retroppTrueUv = uv;
    retroppEvalUv = uv;
    return retroppCustomMain(uv);
}
")
    file(READ "${SPRITE_BASE}" _sprite_base_text)
    string(REPLACE "// @retropp:sprite-custom-hook" "${_sprite_inject}" _sprite_base_text "${_sprite_base_text}")
    set(_compile_src "${TMP}/${TAG}.wrapped.hlsl")
    file(WRITE "${_compile_src}" "${_sprite_base_text}")
elseif(DEFINED SPRITE_BELOW)
    # ── SPRITE-BELOW-INLINE variant (scene-facing) ────────────────────────────────────────────────
    # Inject the custom body + its scene-reading plumbing into the BELOW sprite fragment (SPRITE_BELOW_BASE)
    # at the `// @retropp:sprite-below-custom-hook` marker, so the game shader distorts / grades the composited
    # SCENE beneath the sprite's layer through the silhouette — one pass per below-custom pipeline, no scaling
    # with the sprite count. sampleSource() reads the SCENE (SPRITE_BELOW_PREAMBLE), not the art; the shader's
    # cbuffer is rewritten to file-scope statics fed from the sprite-effect record's param texels (register k
    # at texel 2 + k) — the SAME lane layout the Layer-scope sprite variant uses. Params are FLOAT-family only
    # (the record store is a float texture); an int / uint field is a hard error (declare // @retropp:no-sprite).
    file(READ "${SPRITE_BELOW_PREAMBLE}" _sb_preamble_text)
    file(READ "${SRC}" _sb_body_text)
    # main → retroppCustomMain (a non-entry function; its parameter / return semantics are ignored).
    string(REGEX REPLACE "float4[ \t\r\n]+main[ \t\r\n]*\\(" "float4 retroppCustomMain("
           _sb_body_text "${_sb_body_text}")
    # Strip // comments before the cbuffer parse (a comment may contain the word "cbuffer").
    string(REGEX REPLACE "//[^\n]*" "" _sb_body_text "${_sb_body_text}")
    # cbuffer field parse — MIRRORS gen_effect_fields.cmake (same 16-byte register straddle) so the loader's
    # record texels match the C++ packer's bytes, which the renderer uploads verbatim (identical to SPRITE).
    set(_sb_statics "")
    set(_sb_loader "")
    set(_sb_offset 0)
    if(_sb_body_text MATCHES "cbuffer[^{]*{([^}]*)}")
        set(_sb_cb_body "${CMAKE_MATCH_1}")
        string(REPLACE ";" ";;" _sb_cb_body "${_sb_cb_body}")
        string(REGEX MATCHALL "[A-Za-z0-9_]+[ \t\r\n]+[A-Za-z_][A-Za-z0-9_]*" _sb_decls "${_sb_cb_body}")
        foreach(_sb_decl ${_sb_decls})
            string(REGEX MATCH "^([A-Za-z0-9_]+)[ \t\r\n]+([A-Za-z_][A-Za-z0-9_]*)$" _sb_ok "${_sb_decl}")
            if(NOT _sb_ok)
                continue()
            endif()
            set(_sb_htype "${CMAKE_MATCH_1}")
            set(_sb_sname "${CMAKE_MATCH_2}")
            if(_sb_htype STREQUAL "float")
                set(_sb_ssize 4)
                set(_sb_sncomp 1)
            elseif(_sb_htype STREQUAL "float2")
                set(_sb_ssize 8)
                set(_sb_sncomp 2)
            elseif(_sb_htype STREQUAL "float3")
                set(_sb_ssize 12)
                set(_sb_sncomp 3)
            elseif(_sb_htype STREQUAL "float4")
                set(_sb_ssize 16)
                set(_sb_sncomp 4)
            else()
                message(FATAL_ERROR "gen_shader.cmake SPRITE_BELOW: sprite-path custom params are float-typed "
                                    "(float / float2 / float3 / float4); field '${_sb_sname}' is '${_sb_htype}'. "
                                    "Declare // @retropp:no-sprite to keep this shader off the sprite path.")
            endif()
            math(EXPR _sb_reg_start "${_sb_offset} - (${_sb_offset} % 16)")
            math(EXPR _sb_reg_end "(${_sb_offset} + ${_sb_ssize} - 1) - ((${_sb_offset} + ${_sb_ssize} - 1) % 16)")
            if(NOT _sb_reg_start EQUAL _sb_reg_end)
                math(EXPR _sb_offset "((${_sb_offset} + 15) / 16) * 16")
            endif()
            math(EXPR _sb_sreg "${_sb_offset} / 16")
            math(EXPR _sb_slane "(${_sb_offset} % 16) / 4")
            string(SUBSTRING "xyzw" ${_sb_slane} ${_sb_sncomp} _sb_ssw)
            math(EXPR _sb_stexel "2 + ${_sb_sreg}")     # param register k lives at record texel 2 + k
            string(APPEND _sb_statics "static ${_sb_htype} ${_sb_sname};\n")
            string(APPEND _sb_loader "    ${_sb_sname} = uFxStore.Load(int3(${_sb_stexel}, ri, 0)).${_sb_ssw};\n")
            math(EXPR _sb_offset "${_sb_offset} + ${_sb_ssize}")
        endforeach()
    endif()
    # Remove the shader's own cbuffer block (its params are the statics above).
    string(REGEX REPLACE "cbuffer[^{]*{[^}]*}[ \t\r\n]*;?" "" _sb_body_text "${_sb_body_text}")
    # EMISSION_CONSUMER: an emission-declared stage's below variant reads its blurred field through
    # sampleEmission(uv) — the atlas read (sampleEmissionField, already in scope at the hook) at the field row
    # the seat wrote into the record's GATE lanes (texel 1: radius = row, strokeWidth = engaged). The game body
    # calls sampleEmission in its OWN sampling space (screen uv); the injected function maps uv -> compose px
    # (uv * composeDim). A non-consumer below variant declares none of this and is byte-identical to before.
    set(_sb_emission_decls "")
    set(_sb_emission_wrap "")
    if(DEFINED EMISSION_CONSUMER)
        # composeDim is computed from the viewport statics (set below) × uComposeScale (the below fragment's
        # cbuffer field, in scope at the hook), so the wrapper signature is unchanged and every non-consumer
        # below variant stays byte-identical.
        set(_sb_emission_decls "
static float  retroppEmissionIndex;
static float  retroppEmissionEngaged;
float4 sampleEmission(float2 uv) {
    return retroppEmissionEngaged != 0.0f
        ? sampleEmissionField(uv * float2(uViewportW, uViewportH) * uComposeScale, retroppEmissionIndex)
        : float4(0.0f, 0.0f, 0.0f, 0.0f);
}
")
        set(_sb_emission_wrap "    float4 rpGate = uFxStore.Load(int3(1, ri, 0));  // alpha, field row, engaged, _
    retroppEmissionIndex   = rpGate.y;
    retroppEmissionEngaged = rpGate.z;
")
    endif()
    # The injected block: define RETROPP_SPRITE_BELOW_CUSTOM (compiles out the built-in path + skips the base
    # no-op), the below preamble (sampleSource over the scene), the emission read (consumers only), the param
    # statics + record-lane loader, the custom body, then the wrapper the below fragment calls (reads the step's
    # edge + viewport dims, seats the emission field row, loads the params, evaluates the body at the snapped
    # screen uv).
    set(_sb_inject "
#define RETROPP_SPRITE_BELOW_CUSTOM
${_sb_preamble_text}
${_sb_emission_decls}${_sb_statics}void retroppLoadSpriteParams(int ri) {
${_sb_loader}}
${_sb_body_text}
float4 retroppSpriteBelowCustom(float2 sceneUv, float2 viewportDim, int ri) {
    float4 rpHead = uFxStore.Load(int3(0, ri, 0));                       // kind, flags, blend, pointCount
    uEdgeClamp = (((uint)rpHead.y & 4u) != 0u) ? 1u : 0u;               // the step's edge (Blank / Stretch)
    uViewportW = viewportDim.x;
    uViewportH = viewportDim.y;
${_sb_emission_wrap}    retroppLoadSpriteParams(ri);
    retroppTrueUv = sceneUv;
    retroppEvalUv = (uSnap != 0u) ? retroppSnapToCellCenter(sceneUv) : sceneUv;
    return retroppCustomMain(retroppEvalUv);
}
")
    file(READ "${SPRITE_BELOW_BASE}" _sb_base_text)
    string(REPLACE "// @retropp:sprite-below-custom-hook" "${_sb_inject}" _sb_base_text "${_sb_base_text}")
    set(_compile_src "${TMP}/${TAG}.wrapped.hlsl")
    file(WRITE "${_compile_src}" "${_sb_base_text}")
elseif(DEFINED EMISSION_RECT)
    # ── EMISSION-RECT variant (below WRITE) ─────────────────────────────────────────────────────────
    # Inject the emission-declared shader's `emission()` body into the rect-instanced below extract fragment
    # (EMISSION_RECT_BASE) at `@retropp:emission-rect-hook`, so a Below-scope Custom lens's field is authored by
    # the game's own emission() over the field's rect — the SAME body the frame-class EMISSION_EXTRACT runs,
    # here reading the scene through the rect preamble's sampleSource (EMISSION_RECT_PREAMBLE). The shader's
    # cbuffer is rewritten to file-scope statics fed from the fx record lanes (register k at texel 2 + k, the
    # SPRITE loader). Float params only. An extract cannot read emission (circular), so a sampleEmission() STUB
    # is supplied for the dead renamed main() that may reference it.
    file(READ "${EMISSION_RECT_PREAMBLE}" _er_preamble_text)
    file(READ "${SRC}" _er_body_text)
    # main → retroppCustomMain (dead here — emission() is the entry — but its parameter / return semantics are
    # ignored on a non-entry function).
    string(REGEX REPLACE "float4[ \t\r\n]+main[ \t\r\n]*\\(" "float4 retroppCustomMain("
           _er_body_text "${_er_body_text}")
    # Strip // comments before the cbuffer parse (a comment may contain the word "cbuffer").
    string(REGEX REPLACE "//[^\n]*" "" _er_body_text "${_er_body_text}")
    # cbuffer field parse — MIRRORS gen_effect_fields.cmake / the SPRITE loader (same 16-byte register straddle)
    # so the loader's record texels match the C++ packer's bytes, which the renderer uploads verbatim.
    set(_er_statics "")
    set(_er_loader "")
    set(_er_offset 0)
    if(_er_body_text MATCHES "cbuffer[^{]*{([^}]*)}")
        set(_er_cb_body "${CMAKE_MATCH_1}")
        string(REPLACE ";" ";;" _er_cb_body "${_er_cb_body}")
        string(REGEX MATCHALL "[A-Za-z0-9_]+[ \t\r\n]+[A-Za-z_][A-Za-z0-9_]*" _er_decls "${_er_cb_body}")
        foreach(_er_decl ${_er_decls})
            string(REGEX MATCH "^([A-Za-z0-9_]+)[ \t\r\n]+([A-Za-z_][A-Za-z0-9_]*)$" _er_ok "${_er_decl}")
            if(NOT _er_ok)
                continue()
            endif()
            set(_er_htype "${CMAKE_MATCH_1}")
            set(_er_sname "${CMAKE_MATCH_2}")
            if(_er_htype STREQUAL "float")
                set(_er_ssize 4)
                set(_er_sncomp 1)
            elseif(_er_htype STREQUAL "float2")
                set(_er_ssize 8)
                set(_er_sncomp 2)
            elseif(_er_htype STREQUAL "float3")
                set(_er_ssize 12)
                set(_er_sncomp 3)
            elseif(_er_htype STREQUAL "float4")
                set(_er_ssize 16)
                set(_er_sncomp 4)
            else()
                message(FATAL_ERROR "gen_shader.cmake EMISSION_RECT: sprite-path custom params are float-typed "
                                    "(float / float2 / float3 / float4); field '${_er_sname}' is '${_er_htype}'. "
                                    "Declare // @retropp:no-sprite to keep this shader off the sprite path.")
            endif()
            math(EXPR _er_reg_start "${_er_offset} - (${_er_offset} % 16)")
            math(EXPR _er_reg_end "(${_er_offset} + ${_er_ssize} - 1) - ((${_er_offset} + ${_er_ssize} - 1) % 16)")
            if(NOT _er_reg_start EQUAL _er_reg_end)
                math(EXPR _er_offset "((${_er_offset} + 15) / 16) * 16")
            endif()
            math(EXPR _er_sreg "${_er_offset} / 16")
            math(EXPR _er_slane "(${_er_offset} % 16) / 4")
            string(SUBSTRING "xyzw" ${_er_slane} ${_er_sncomp} _er_ssw)
            math(EXPR _er_stexel "2 + ${_er_sreg}")     # param register k lives at record texel 2 + k
            string(APPEND _er_statics "static ${_er_htype} ${_er_sname};\n")
            string(APPEND _er_loader "    ${_er_sname} = uFxStore.Load(int3(${_er_stexel}, ri, 0)).${_er_ssw};\n")
            math(EXPR _er_offset "${_er_offset} + ${_er_ssize}")
        endforeach()
    endif()
    # Remove the shader's own cbuffer block (its params are the statics above).
    string(REGEX REPLACE "cbuffer[^{]*{[^}]*}[ \t\r\n]*;?" "" _er_body_text "${_er_body_text}")
    set(_er_inject "
#define RETROPP_EMISSION_RECT_CUSTOM
${_er_preamble_text}
float4 sampleEmission(float2 uv) { return float4(0.0f, 0.0f, 0.0f, 0.0f); }
${_er_statics}void retroppLoadSpriteParams(int ri) {
${_er_loader}}
${_er_body_text}
float4 retroppEmissionRect(float2 sceneUv, float2 viewportDim, int ri) {
    float4 rpHead = uFxStore.Load(int3(0, ri, 0));                       // kind, flags, blend, pointCount
    uEdgeClamp = (((uint)rpHead.y & 4u) != 0u) ? 1u : 0u;               // the step's edge (Blank / Stretch)
    uViewportW = viewportDim.x;
    uViewportH = viewportDim.y;
    retroppLoadSpriteParams(ri);
    retroppTrueUv = sceneUv;
    retroppEvalUv = (uSnap != 0u) ? retroppSnapToCellCenter(sceneUv) : sceneUv;
    return emission(retroppEvalUv);
}
")
    file(READ "${EMISSION_RECT_BASE}" _er_base_text)
    string(REPLACE "// @retropp:emission-rect-hook" "${_er_inject}" _er_base_text "${_er_base_text}")
    set(_compile_src "${TMP}/${TAG}.wrapped.hlsl")
    file(WRITE "${_compile_src}" "${_er_base_text}")
elseif(DEFINED PREAMBLE)
    file(READ "${PREAMBLE}" _preamble_text)
    file(READ "${SRC}" _body_text)
    string(REGEX REPLACE "float4[ \t\r\n]+main[ \t\r\n]*\\(" "float4 retroppCustomMain("
           _body_text "${_body_text}")
    if(DEFINED GATHER)
        # ── GATHER variant ─────────────────────────────────────────────────────────────────────────
        # ONE fullscreen pass collapses N same-stage REPLACE regions. The shader's own cbuffer is rewritten
        # to file-scope statics (so the body's param references resolve unchanged) fed by a generated
        # retroppLoadParams(rec) that reads the WINNING region's params from a storage buffer of per-region
        # records; the entry point tests the fragment against every region's gate (a uv-bbox quick-reject
        # then the n≤2 point-segment SDF, LAST containing region wins) and either passes the source through
        # (outside every shape, byte-exact) or evaluates the body with the winner's params loaded.
        #
        # Strip // comments from the body BEFORE the cbuffer parse AND the cbuffer replacement. A comment
        # may itself contain the word "cbuffer" (effect_probe's does — "Declares a KNOWN cbuffer …"), which
        # the greedy `cbuffer[^{]*{` match below would latch onto, eating the real declaration in between;
        # and the compiler needs no comments. Same precedent as gen_effect_fields.cmake's field parse.
        string(REGEX REPLACE "//[^\n]*" "" _body_text "${_body_text}")
        # cbuffer field parse — MIRRORS gen_effect_fields.cmake exactly (same regex, same 16-byte register
        # straddle rule) so the loader's storage-buffer offsets match the C++ packer's bytes, which the CPU
        # uploads verbatim. The cbuffer is replaced in the body below (now comment-free) with the statics so
        # the body's param references still resolve.
        set(_statics "")
        set(_loader "")
        set(_goffset 0)
        if(_body_text MATCHES "cbuffer[^{]*{([^}]*)}")
            set(_cb_body "${CMAKE_MATCH_1}")
            string(REPLACE ";" ";;" _cb_body "${_cb_body}")
            string(REGEX MATCHALL "[A-Za-z0-9_]+[ \t\r\n]+[A-Za-z_][A-Za-z0-9_]*" _decls "${_cb_body}")
            foreach(_decl ${_decls})
                string(REGEX MATCH "^([A-Za-z0-9_]+)[ \t\r\n]+([A-Za-z_][A-Za-z0-9_]*)$" _ok "${_decl}")
                if(NOT _ok)
                    continue()
                endif()
                set(_htype "${CMAKE_MATCH_1}")
                set(_gname "${CMAKE_MATCH_2}")
                if(_htype STREQUAL "float")
                    set(_gsize 4)
                    set(_gncomp 1)
                    set(_gas "")
                elseif(_htype STREQUAL "float2")
                    set(_gsize 8)
                    set(_gncomp 2)
                    set(_gas "")
                elseif(_htype STREQUAL "float3")
                    set(_gsize 12)
                    set(_gncomp 3)
                    set(_gas "")
                elseif(_htype STREQUAL "float4")
                    set(_gsize 16)
                    set(_gncomp 4)
                    set(_gas "")
                elseif(_htype STREQUAL "int")
                    set(_gsize 4)
                    set(_gncomp 1)
                    set(_gas "asint")
                elseif(_htype STREQUAL "uint")
                    set(_gsize 4)
                    set(_gncomp 1)
                    set(_gas "asuint")
                else()
                    message(FATAL_ERROR "gen_shader.cmake GATHER: unsupported cbuffer field type '${_htype}' "
                                        "(supported: float, float2, float3, float4, int, uint)")
                endif()
                # 16-byte-register straddle rule (identical to gen_effect_fields.cmake).
                math(EXPR _reg_start "${_goffset} - (${_goffset} % 16)")
                math(EXPR _reg_end "(${_goffset} + ${_gsize} - 1) - ((${_goffset} + ${_gsize} - 1) % 16)")
                if(NOT _reg_start EQUAL _reg_end)
                    math(EXPR _goffset "((${_goffset} + 15) / 16) * 16")
                endif()
                math(EXPR _greg "${_goffset} / 16")     # float4 index within the params region
                math(EXPR _glane "(${_goffset} % 16) / 4")  # starting component lane
                string(SUBSTRING "xyzw" ${_glane} ${_gncomp} _gsw)
                string(APPEND _statics "static ${_htype} ${_gname};\n")
                if(_gas STREQUAL "")
                    string(APPEND _loader "    ${_gname} = loadRec(pb + ${_greg}u).${_gsw};\n")
                else()
                    string(APPEND _loader "    ${_gname} = ${_gas}(loadRec(pb + ${_greg}u).${_gsw});\n")
                endif()
                math(EXPR _goffset "${_goffset} + ${_gsize}")
            endforeach()
        endif()
        math(EXPR _gtotal "((${_goffset} + 15) / 16) * 16")
        math(EXPR _gparamf4 "${_gtotal} / 16")
        math(EXPR _gstride "3 + ${_gparamf4}")

        # Replace the shader's own cbuffer (its whole `cbuffer … { … };` block, trailing `;` consumed) with
        # the file-scope statics.
        string(REGEX REPLACE "cbuffer[^{]*{[^}]*}[ \t\r\n]*;?" "${_statics}" _body_text "${_body_text}")

        # The loader body: fill each static from the winning record's param float4s (params start at the
        # record's float4 index 3, after the 3-float4 header). Empty for a parameterless shader (no `pb`).
        if(_loader STREQUAL "")
            set(_loader_body "")
        else()
            set(_loader_body "    uint pb = rec * kGatherStride + 3u;\n${_loader}")
        endif()

        set(_gather_decls "
ByteAddressBuffer RegionRecords : register(t2, space2);  // packed float4 records; byte address = idx*16 (loadRec — SDL leaves the D3D12 StructureByteStride 0, which AMD uses to index a StructuredBuffer, collapsing every dynamic index to element 0)
float4 loadRec(uint idx) { return asfloat(RegionRecords.Load4(idx * 16u)); }
cbuffer RetroppGatherInfo : register(b1, space3) {
    uint uRegionCount;
    uint uGatherPad0;
    uint uGatherPad1;
    uint uGatherPad2;
};
static const uint kGatherStride = ${_gstride}u;
float retroppGatherPointSegmentDistance(float2 p, float2 a, float2 b) {
    float2 ab = b - a;
    float2 ap = p - a;
    float ee = dot(ab, ab);
    float t = ee > 0.0f ? clamp(dot(ap, ab) / ee, 0.0f, 1.0f) : 0.0f;
    float2 q = a + ab * t;
    return length(p - q);
}
void retroppLoadParams(uint rec) {
${_loader_body}}
")
        # The gather entry point: mirrors region_select.frag's n≤2 gate + the CRISP snap of the test point.
        # Header float4s per record: 0 = uv bbox (quick-reject), 1 = spine (viewport px), 2.x = radius. LAST
        # containing region wins. No hit → sample the source directly at the true uv (byte-exact
        # passthrough — the region_select.frag source-side precedent, bypassing the quantizing sampleSource).
        set(_gather_trampoline "
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    retroppTrueUv = uv;
    retroppEvalUv = (uSnap != 0u) ? retroppSnapToCellCenter(uv) : uv;
    float2 fragPx = uv * float2(uViewportW, uViewportH);
    if (uSnap != 0u) fragPx = floor(fragPx) + 0.5f;
    int winner = -1;
    for (uint i = 0u; i < uRegionCount; ++i) {
        uint base = i * kGatherStride;
        float4 box = loadRec(base);
        if (uv.x < box.x || uv.y < box.y || uv.x > box.z || uv.y > box.w) continue;
        float4 spine = loadRec(base + 1u);
        float rad = loadRec(base + 2u).x;
        if (retroppGatherPointSegmentDistance(fragPx, spine.xy, spine.zw) - rad <= 0.0f) winner = int(i);
    }
    if (winner < 0) return SourceTexture.Sample(SourceSampler, retroppTrueUv);
    retroppLoadParams(uint(winner));
    return retroppCustomMain(retroppEvalUv);
}
")
        set(_compile_src "${TMP}/${TAG}.wrapped.hlsl")
        file(WRITE "${_compile_src}"
             "${_preamble_text}\n${_body_text}\n${_gather_decls}\n${_gather_trampoline}")
    else()
        if(DEFINED BATCHED)
            # The batched entry point: the instanced quad hands three varyings (the frame-global uv, the
            # region's SDF spine, and its radius). It replicates the region gate exactly for the n≤2 shapes
            # (point-segment distance ≤ radius; a circle has p1 == p0) — discarding outside — then returns the
            # shader body evaluated at the snapped cell center. With a zero source bound, sampleSource() returns
            # 0 everywhere, so the body returns exactly its source-independent additive term D(uv); hardware
            # additive blending accumulates D onto the destination. The snap mirrors region_select.frag.
            set(_trampoline "
float retroppPointSegmentDistance(float2 p, float2 a, float2 b) {
    float2 ab = b - a;
    float2 ap = p - a;
    float ee = dot(ab, ab);
    float t = ee > 0.0f ? clamp(dot(ap, ab) / ee, 0.0f, 1.0f) : 0.0f;
    float2 q = a + ab * t;
    return length(p - q);
}
float4 main(float2 uv : TEXCOORD0, float4 spine : TEXCOORD1, float2 rad : TEXCOORD2) : SV_Target0 {
    retroppTrueUv = uv;
    retroppEvalUv = (uSnap != 0u) ? retroppSnapToCellCenter(uv) : uv;
    float2 fragPx = uv * float2(uViewportW, uViewportH);
    if (uSnap != 0u) fragPx = floor(fragPx) + 0.5f;
    if (retroppPointSegmentDistance(fragPx, spine.xy, spine.zw) - rad.x > 0.0f) discard;
    return retroppCustomMain(retroppEvalUv);
}
")
        elseif(DEFINED EMISSION_EXTRACT)
            # ── EMISSION EXTRACT variant ─────────────────────────────────────────────────────────────
            # The emission-declared shader's `float4 emission(float2 uv)` body IS the entry here: the engine
            # runs it in the extract pass over the field's rect, reading the composited scene through
            # sampleSource() exactly as the stock brightpass does. The result is blurred and handed back to
            # main() (the fullscreen variant) through sampleEmission(). The extract compiles at the STOCK
            # single-sampler layout — an extract cannot read emission (that would be circular), so no
            # RETROPP_EMISSION define. The renamed main() (retroppCustomMain) is dead here but still
            # compiles; it may reference sampleEmission(), so a returns-nothing stub is supplied.
            set(_trampoline "
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    retroppTrueUv = uv;
    retroppEvalUv = (uSnap != 0u) ? retroppSnapToCellCenter(uv) : uv;
    return emission(retroppEvalUv);
}
")
        else()
            set(_trampoline "
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    retroppTrueUv = uv;
    retroppEvalUv = (uSnap != 0u) ? retroppSnapToCellCenter(uv) : uv;
    return retroppCustomMain(retroppEvalUv);
}
")
        endif()
        set(_compile_src "${TMP}/${TAG}.wrapped.hlsl")
        # EMISSION (the main variant of an emission-declared stage): compile the standard preamble with
        # RETROPP_EMISSION so EmissionTexture (t1/s1) + sampleEmission() are declared and RowDataTexture
        # moves to t2. Undeclared stages define nothing, so the preamble takes its byte-identical #else
        # branch. EMISSION_EXTRACT compiles WITHOUT the define (stock layout) but supplies a sampleEmission
        # stub so the dead retroppCustomMain compiles.
        set(_emission_prefix "")
        if(DEFINED EMISSION)
            set(_emission_prefix "#define RETROPP_EMISSION 1\n")
        elseif(DEFINED EMISSION_EXTRACT)
            set(_emission_prefix "float4 sampleEmission(float2 uv) { return float4(0.0f, 0.0f, 0.0f, 0.0f); }\n")
        endif()
        file(WRITE "${_compile_src}" "${_emission_prefix}${_preamble_text}\n${_body_text}\n${_trampoline}")
    endif()
endif()

function(run_tool)
    execute_process(COMMAND ${ARGV}
                    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        list(JOIN ARGV " " _cmd)
        message(FATAL_ERROR "gen_shader.cmake: command failed (${_rc}): ${_cmd}\n${_out}\n${_err}")
    endif()
endfunction()

# Render a lowercase hex string as an `inline constexpr unsigned char` array
# definition, 12 bytes per row, into ${out_var}. The 24-dot group is 12 hex byte
# pairs — CMake's regex flavor has no {n} bounded repetition.
function(hex_to_byte_array name hex out_var)
    string(REGEX REPLACE "(........................)" "\\1\n" _rows "${hex}")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " _rows "${_rows}")
    string(REGEX REPLACE ", \n" ",\n    " _rows "${_rows}")
    string(REGEX REPLACE ", $" "," _rows "${_rows}")
    set(${out_var} "inline constexpr unsigned char ${name}[] = {\n    ${_rows}\n};" PARENT_SCOPE)
endfunction()

# SPIR-V / DXIL keep "main"; spirv-cross renames the Metal entry ("main" is reserved
# in MSL — the SPIRV-Cross default is main0, detected from the emitted source below).
# The metallib exposes that same renamed function name.
set(ENTRY_SPIRV "main")
set(ENTRY_DXIL "main")
set(ENTRY_METALLIB "main0")

if(FORMAT STREQUAL "spirv" OR FORMAT STREQUAL "metallib")
    set(_spv "${TMP}/${TAG}.spv")
    run_tool("${GLSLANG}" -D -e main -S "${STAGE}" --target-env vulkan1.2
             ${_inc_glslang} -o "${_spv}" "${_compile_src}")
    if(FORMAT STREQUAL "spirv")
        file(READ "${_spv}" _hex HEX)
        hex_to_byte_array(kBytes "${_hex}" _array)
    else()
        # metallib: spirv-cross emits MSL (the intermediate), then the Metal toolchain
        # compiles it offline to a precompiled library — so no Metal shader frontend runs
        # on the player's machine at pipeline creation. The .metal extension is how `metal`
        # keys its input; the bytes are exactly what spirv-cross wrote.
        set(_msl "${TMP}/${TAG}.metal")
        # --msl-decoration-binding: assign [[texture(n)]]/[[buffer(n)]] indices from
        # the SPIR-V binding decorations (register index), matching the slot order
        # SDL_GPU binds resources in. Without it spirv-cross allocates indices in
        # variable-ID order, which can silently swap same-set resources (observed:
        # tile.frag uAtlas/uTilemap swapped → atlas bytes fed to the tilemap slot).
        run_tool("${SPIRV_CROSS}" "${_spv}" --msl --msl-version 20000
                 --msl-decoration-binding --output "${_msl}")
        file(READ "${_msl}" _msl_text)
        # MSL reserves "main", so spirv-cross renames the entry (default main0). The metallib
        # exposes that same function name — detect it from the MSL and pass it as the metallib
        # entrypoint (SDL resolves the shader function by name in the library).
        if(_msl_text MATCHES "(vertex|fragment|kernel)[ \t]+[A-Za-z0-9_]+[ \t]+([A-Za-z0-9_]+)[ \t]*\\(")
            set(ENTRY_METALLIB "${CMAKE_MATCH_2}")
        endif()
        # MSL → AIR → metallib via the Metal toolchain. The shipped artifact is the metallib
        # BINARY (no NUL terminator; the variant size is the file's byte length).
        set(_air "${TMP}/${TAG}.air")
        set(_metallib "${TMP}/${TAG}.metallib")
        run_tool(xcrun -sdk macosx metal -c "${_msl}" -o "${_air}")
        run_tool(xcrun -sdk macosx metallib "${_air}" -o "${_metallib}")
        file(READ "${_metallib}" _hex HEX)
        hex_to_byte_array(kBytes "${_hex}" _array)
    endif()
elseif(FORMAT STREQUAL "dxil")
    set(_dxil "${TMP}/${TAG}.dxil")
    if(STAGE STREQUAL "vert")
        set(_profile vs_6_0)
    else()
        set(_profile ps_6_0)
    endif()
    run_tool("${DXC}" -T "${_profile}" -E main ${_inc_dxc} -Fo "${_dxil}" "${_compile_src}")
    file(READ "${_dxil}" _hex HEX)
    hex_to_byte_array(kBytes "${_hex}" _array)
else()
    message(FATAL_ERROR "gen_shader.cmake: unknown FORMAT '${FORMAT}'")
endif()

# The header exposes ONE ready-to-use symbol: `retropp::shaders::<stem>`, a constexpr
# ShaderVariants the engine and any game register directly — no hand-assembly, no per-
# shader namespace. The platform's built format carries the bytes (in a <stem>_detail
# namespace); the other two slots are absent variants (default ShaderBytecode — null
# data), which selectShader skips, and a device never reports a format its backend
# doesn't run.
set(_real "${NS}_detail::kBytes, sizeof(${NS}_detail::kBytes)")
if(FORMAT STREQUAL "spirv")
    set(_spirv_slot "{${_real}, \"${ENTRY_SPIRV}\"}")
    set(_dxil_slot "{}")
    set(_metallib_slot "{}")
elseif(FORMAT STREQUAL "dxil")
    set(_spirv_slot "{}")
    set(_dxil_slot "{${_real}, \"${ENTRY_DXIL}\"}")
    set(_metallib_slot "{}")
else()
    set(_spirv_slot "{}")
    set(_dxil_slot "{}")
    set(_metallib_slot "{${_real}, \"${ENTRY_METALLIB}\"}")
endif()

file(WRITE "${OUT}"
    "#pragma once\n"
    "// AUTO-GENERATED at build time by shaders/gen_shader.cmake for this platform's GPU\n"
    "// format (${FORMAT}). DO NOT EDIT and DO NOT COMMIT — it is a build artifact.\n"
    "// Source: ${SRC}\n\n"
    "#include \"retropp/shader_format.h\"\n\n"
    "namespace retropp::shaders {\n\n"
    "namespace ${NS}_detail {\n"
    "${_array}\n"
    "}  // namespace ${NS}_detail\n\n"
    "// The compiled shader in every backend format the engine ships. Engine-internal stages consume this\n"
    "// directly; a game's custom stage is registered BY PATH (renderer.registerPostProcessStage(\"...\"))\n"
    "// and resolved here automatically — see retropp_autocompile_shaders / shader_registry.h.\n"
    "inline constexpr ShaderVariants ${NS}{\n"
    "    ${_spirv_slot},  // spirv    (Vulkan)\n"
    "    ${_dxil_slot},  // dxil     (Direct3D 12)\n"
    "    ${_metallib_slot}};  // metallib (Metal)\n\n"
    "}  // namespace retropp::shaders\n")

message(STATUS "gen_shader: ${OUT} (${FORMAT})")
