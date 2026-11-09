// Underwater light rays: sunlight scattered in the water toward the camera, in moving shafts where
// the waves focus it. Linked with fsCausticsF.glsl, so the shafts continue the caustic pattern on
// the floor. Only drawn while the camera is under water.

/*[EXTRA_CODE_HERE]*/

// Written at reduced resolution into two channels, as fsSunRaysF: r is how much sunlight is
// scattered toward the camera along the ray to this pixel, before the light's colour, g is the
// distance to what this pixel sees. fsSunRaysCompositeF filters it back up and adds it.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2DShadow shadowMap0;
uniform sampler2DShadow shadowMap1;
uniform sampler2DShadow shadowMap2;
uniform sampler2DShadow shadowMap3;

uniform mat4 shadow_matrix[6];
uniform vec4 shadow_clip;
uniform float shadow_bias;

uniform vec3 sun_dir;               // view space, toward the light
uniform vec4 waterPlane;            // view space water plane, positive above the surface

uniform int ray_steps;
uniform float ray_max_dist;         // metres, the march never goes past this
uniform float ray_density;          // scattering per metre
uniform float ray_extinction;       // light lost per metre of water (scattering and absorption)
uniform float ray_anisotropy;       // forward scattering, 0 even
uniform float ray_shafts;           // 0 even light, 1 full caustic shafts

// fsCausticsF.glsl
uniform vec4 fs_caustic_params;     // x unused here, y cells per metre, z animation phase
uniform mat4 fs_caustic_inv_view;   // eye space to agent space
float fsCausticLayer(vec2 p, float t);

// Distance stored for the sky, as in fsSunRaysF
const float SKY_DIST = 65000.0;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

// Lit (1) or shadowed (0) at a view space position, as in fsSunRaysF
float sunVisibility(vec3 pos)
{
    float dist = -pos.z;
    vec4 p = vec4(pos, 1.0);
    vec4 lpos;
    float vis = 1.0;
    if (dist < shadow_clip.x)
    {
        lpos = shadow_matrix[0] * p;
        lpos.xyz /= lpos.w;
        vis = texture(shadowMap0, vec3(lpos.xy, lpos.z + shadow_bias));
    }
    else if (dist < shadow_clip.y)
    {
        lpos = shadow_matrix[1] * p;
        lpos.xyz /= lpos.w;
        vis = texture(shadowMap1, vec3(lpos.xy, lpos.z + shadow_bias));
    }
    else if (dist < shadow_clip.z)
    {
        lpos = shadow_matrix[2] * p;
        lpos.xyz /= lpos.w;
        vis = texture(shadowMap2, vec3(lpos.xy, lpos.z + shadow_bias));
    }
    else if (dist < shadow_clip.w)
    {
        lpos = shadow_matrix[3] * p;
        lpos.xyz /= lpos.w;
        vis = texture(shadowMap3, vec3(lpos.xy, lpos.z + shadow_bias));
    }
    return vis;
}

// Henyey-Greenstein, scaled so that even scattering is 1, capped as in fsSunRaysF
float phase(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return min((1.0 - g2) / (denom * sqrt(denom)), 16.0);
}

// Per pixel start offset, as in fsSunRaysF
float stepOffset(vec2 frag)
{
    return fract(52.9829189 * fract(dot(frag, vec2(0.06711056, 0.00583715))));
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float depth = getDepth(tc);
    bool sky = depth >= 1.0;

    vec3 pos = getPositionWithDepth(tc, sky ? 0.5 : depth).xyz;
    vec3 view_dir = normalize(pos);
    float scene_dist = sky ? SKY_DIST : min(length(pos), SKY_DIST);

    vec3 up = waterPlane.xyz;
    float cam_below = -waterPlane.w;            // metres the camera is under the surface
    float sun_height = dot(normalize(sun_dir), up);   // sine of the sun's elevation
    float march_dist = min(scene_dist, ray_max_dist);
    float rise = dot(view_dir, up);             // metres gained in height per metre along the ray
    if (rise > 0.0)
    {
        march_dist = min(march_dist, cam_below / rise);   // up to where the ray leaves the water
    }

    if (cam_below <= 0.0 || sun_height <= 0.0 || march_dist <= 0.0)
    {
        frag_color = vec4(0.0, scene_dist, 0.0, 1.0);
        return;
    }

    // The sun's path in the water is steeper than in the air (refraction); as fsWaterCaustics
    vec3 wlight = mat3(fs_caustic_inv_view) * normalize(sun_dir);
    vec2 slide = wlight.xy / max(wlight.z, 0.2) * 0.75;    // metres across per metre of depth
    float path_per_depth = 1.0 / max(sun_height, 0.2) * 0.85;
    float anim = fs_caustic_params.z;
    float cells = fs_caustic_params.y;

    int steps = max(ray_steps, 1);
    float inv_steps = 1.0 / float(steps);
    float jitter = stepOffset(gl_FragCoord.xy);

    float scattered = 0.0;
    float optical_depth = 0.0;
    float prev_t = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        float u = (float(i) + jitter) * inv_steps;
        float t = march_dist * u * u;
        float dt = 2.0 * march_dist * u * inv_steps;
        vec3 p = view_dir * t;
        float below = max(cam_below - t * rise, 0.0);

        // Back toward the camera through the water in between
        optical_depth += ray_extinction * (t - prev_t);
        prev_t = t;

        // Sunlight left at this depth, after its slanted path down from the surface
        float sunlight = exp(-ray_extinction * below * path_per_depth);

        // Where this point's sunlight came through the surface: the waves focused it into the
        // caustic web there, and that web, carried down along the sun's path, is the shafts.
        // The focus spreads with depth, so the shafts soften into even light further down.
        vec2 xy = (fs_caustic_inv_view * vec4(p, 1.0)).xy + slide * below;
        vec2 cp = xy * cells;
        float c = fsCausticLayer(cp, anim) * 0.6
                + fsCausticLayer(mat2(0.8, -0.6, 0.6, 0.8) * cp * 1.7 + vec2(3.1, 7.7), 2.0 * anim) * 0.4;
        float focus = ray_shafts * exp(-0.06 * below);
        float shaft = mix(1.0, 0.2 + 3.0 * c, focus);

        scattered += sunVisibility(p) * sunlight * shaft * ray_density * exp(-optical_depth) * dt;
    }

    float cos_theta = dot(view_dir, normalize(sun_dir));
    float result = scattered * phase(cos_theta, ray_anisotropy);
    frag_color = vec4(result, scene_dist, 0.0, 1.0);
}
