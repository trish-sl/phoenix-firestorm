/**
 * @file objectSkinV.glsl
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

in vec4 weight4;

uniform mat3x4 matrixPalette[MAX_JOINTS_PER_MESH_OBJECT];
// Number of valid entries uploaded for this mesh.  Keeping this separate from
// MAX_JOINTS_PER_MESH_OBJECT prevents malformed/partially loaded rig weights
// from reading stale uniform-array entries beyond the uploaded palette.
uniform int matrixPaletteSize;

mat4 getObjectSkinnedTransform()
{
    int i;

    vec4 w = fract(weight4);
    vec4 index = floor(weight4);

    int last_palette_index = min(max(matrixPaletteSize - 1, 0), MAX_JOINTS_PER_MESH_OBJECT - 1);
    index = min(index, vec4(float(last_palette_index)));
    index = max(index, vec4( 0.0));

    float weight_sum = w.x + w.y + w.z + w.w;
    if (weight_sum > 0.0)
    {
        w *= 1.0 / weight_sum;
    }
    else
    {
        // Broken or partially decoded weights must not turn the skin matrix
        // into NaNs.  Keep the vertex attached to the first valid joint.
        w = vec4(1.0, 0.0, 0.0, 0.0);
    }

    int i1 = int(index.x);
    int i2 = int(index.y);
    int i3 = int(index.z);
    int i4 = int(index.w);

    mat3 mat = mat3(matrixPalette[i1])*w.x;
         mat += mat3(matrixPalette[i2])*w.y;
         mat += mat3(matrixPalette[i3])*w.z;
         mat += mat3(matrixPalette[i4])*w.w;

    vec3 trans = vec3(matrixPalette[i1][0].w,matrixPalette[i1][1].w,matrixPalette[i1][2].w)*w.x;
         trans += vec3(matrixPalette[i2][0].w,matrixPalette[i2][1].w,matrixPalette[i2][2].w)*w.y;
         trans += vec3(matrixPalette[i3][0].w,matrixPalette[i3][1].w,matrixPalette[i3][2].w)*w.z;
         trans += vec3(matrixPalette[i4][0].w,matrixPalette[i4][1].w,matrixPalette[i4][2].w)*w.w;

    mat4 ret;

    ret[0] = vec4(mat[0], 0);
    ret[1] = vec4(mat[1], 0);
    ret[2] = vec4(mat[2], 0);
    ret[3] = vec4(trans, 1.0);

#ifdef IS_AMD_CARD
   // If it's AMD make sure the GLSL compiler sees the arrays referenced once by static index. Otherwise it seems to optimise the storage awawy which leads to unfun crashes and artifacts.
   mat3x4 dummy1 = matrixPalette[0];
   mat3x4 dummy2 = matrixPalette[MAX_JOINTS_PER_MESH_OBJECT-1];
#endif

    return ret;
}
