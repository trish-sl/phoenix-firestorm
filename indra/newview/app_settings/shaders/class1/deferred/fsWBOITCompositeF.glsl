// Weighted blended order-independent transparency: composite the accumulated alpha over the scene

/*[EXTRA_CODE_HERE]*/

// McGuire and Bavoil 2013. The alpha shaders built with FS_WBOIT added, per pixel,
//   diffuseRect.rgb  = sum(colour * alpha * weight)
//   diffuseRect.a    = product(1 - alpha), the revealage
//   specularRect.r   = sum(alpha * weight)
// Drawn over the HDR scene with (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) on colour and
// (ZERO, ONE_MINUS_SRC_ALPHA) on alpha, so the scene shows through by the revealage and the
// glow in the screen's alpha is dimmed by it, as the sorted blend does.

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform sampler2D specularRect;

void main()
{
    ivec2 tc = ivec2(gl_FragCoord.xy);
    vec4 accum = texelFetch(diffuseRect, tc, 0);
    float revealage = accum.a;

    if (revealage >= 0.9999)
    { // nothing transparent covers this pixel
        discard;
    }

    float weight = texelFetch(specularRect, tc, 0).r;
    // A half float sum can overflow to infinity with many bright layers close to the camera
    vec3 color = min(accum.rgb, vec3(65000.0)) / max(weight, 1e-8);

    frag_color = vec4(clamp(color, vec3(0.0), vec3(65000.0)), clamp(1.0 - revealage, 0.0, 1.0));
}
