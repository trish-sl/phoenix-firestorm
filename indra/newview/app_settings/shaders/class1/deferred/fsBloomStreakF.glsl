// Anamorphic streaks: a horizontal blur of the bloom's bright pass, run three times with a
// growing step (1, 5 and 25 texels) so a bright point becomes a long, thin, tapering line, as
// the flare of an anamorphic cinema lens. The last run (step 0) adds the result, tinted, into
// the summed bloom, which the tonemap pass then adds to the scene.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;  // the bright pass, or the previous streak pass
uniform vec2 src_texel;         // one texel of diffuseRect
uniform float streak_step;      // tap spacing in texels, 0 for the final add
uniform vec3 streak_gain;       // tint times strength

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec2 step_tc = vec2(streak_step * src_texel.x, 0.0);

    // Nine taps, falling off exponentially from the centre; normalised, so each run only spreads
    vec3 c = vec3(0.0);
    float total = 0.0;
    for (int k = -4; k <= 4; ++k)
    {
        float w = exp(-0.45 * abs(float(k)));
        c += texture(diffuseRect, tc + step_tc * float(k)).rgb * w;
        total += w;
    }

    frag_color = vec4(c / total * streak_gain, 1.0);
}
