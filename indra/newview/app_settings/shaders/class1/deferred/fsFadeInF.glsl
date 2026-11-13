// Fade-in: newly arrived objects dissolve into view instead of popping in

// Linked as an extra fragment object into the world G-buffer and alpha programs when FS_FADE_IN
// is defined (FSRenderFadeIn); those shaders call fsFadeIn first thing in main.
//
// A screen-door dissolve, as games fade in streamed objects: while part of the object is still
// hidden, a fragment is discarded when its threshold in a fixed interleaved gradient noise pattern
// (Jimenez 2014) is below that part. The object thins in evenly without blending, so the opaque
// G-buffer passes keep their depth writes and order, and its shadows and reflections are untouched.

// The part of the object being drawn that is still hidden: 1 just arrived, 0 fully shown. Set per
// batch by LLRenderPass::applyModelMatrix; programs that never set it (GPU-driven multi-draws, which
// leave fading batches to the stock path) read 0 and show everything.
uniform float fs_fade_hide;

void fsFadeIn()
{
    if (fs_fade_hide > 0.0)
    {
        float threshold = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        if (threshold < fs_fade_hide)
        {
            discard;
        }
    }
}
