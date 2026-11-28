/**
 * @file llhudnametag.cpp
 * @brief Name tags for avatars
 * @author James Cook
 *
 * $LicenseInfo:firstyear=2010&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

#include "llviewerprecompiledheaders.h"

#include "llhudnametag.h"

#include "llrender.h"
#include "lltracerecording.h"

#include "llagent.h"
#include "llviewercontrol.h"
#include "llcriticaldamp.h"
#include "lldrawable.h"
#include "llfontgl.h"
#include "llglheaders.h"
#include "llhudrender.h"
#include "llui.h"
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "llviewertexturelist.h"
#include "llviewerobject.h"
#include "llviewerwindow.h"
#include "llstatusbar.h"
#include "llmenugl.h"
#include "pipeline.h"
#include <boost/tokenizer.hpp>
#include "llavatarpropertiesprocessor.h" // <FS:Trish/> pill style name tags: profile pictures
#include <glm/gtc/matrix_transform.hpp> // <FS:Trish/> pill style name tags: glm::project


const F32 SPRING_STRENGTH = 0.7f;
const F32 HORIZONTAL_PADDING = 16.f;
const F32 VERTICAL_PADDING = 12.f;
const F32 LINE_PADDING = 3.f;           // aka "leading"
const F32 BUFFER_SIZE = 2.f;
const S32 NUM_OVERLAP_ITERATIONS = 10;
const F32 POSITION_DAMPING_TC = 0.2f;
const F32 MAX_STABLE_CAMERA_VELOCITY = 0.1f;
const F32 LOD_0_SCREEN_COVERAGE = 0.15f;
const F32 LOD_1_SCREEN_COVERAGE = 0.30f;
const F32 LOD_2_SCREEN_COVERAGE = 0.40f;

std::set<LLPointer<LLHUDNameTag> > LLHUDNameTag::sTextObjects;
std::vector<LLPointer<LLHUDNameTag> > LLHUDNameTag::sVisibleTextObjects;
bool LLHUDNameTag::sDisplayText = true ;
const F32 LLHUDNameTag::NAMETAG_MAX_WIDTH = 298.f;
const F32 LLHUDNameTag::HUD_TEXT_MAX_WIDTH = 190.f;

// <FS:Trish> Pill style name tags (FSNameTagPillStyle). A rounded pill sized to the text, with less
// padding than the stock bubble, and on its left, optionally (FSNameTagProfilePictures), the avatar's
// profile picture filling the round end. Without a picture, a dot tells the chat range they are in
// (FSNameTagChatRangeRing): green within whisper range, yellow within chat range, red within shout range.
namespace
{
    constexpr F32 FS_PILL_PAD_TOP = 2.f;
    constexpr F32 FS_PILL_PAD_BOTTOM = 2.f;
    constexpr F32 FS_PILL_LINE_PAD = 0.f;
    constexpr F32 FS_PILL_GAP = 5.f;            // picture or dot to the text
    constexpr F32 FS_PILL_DOT = 7.f;            // range dot without a picture
    constexpr F32 FS_PILL_MIN_PICTURE = 14.f;   // smaller than this, no picture
    constexpr F32 FS_PILL_FULL_SIZE_DIST = 5.f; // full size up to this far (m)
    constexpr F32 FS_PILL_SHRINK_SECONDS = 0.3f; // to shrink into the picture, or grow back out of it
    constexpr F32 FS_PILL_SCALE_STEP = 0.1f;    // a tag shrinks with distance in steps, each with its own font size
    constexpr F32 FS_PILL_SCALE_HYSTERESIS = 0.02f; // past the half way point between steps before switching

    // eased in and out, for the shrink into the picture
    inline F32 fs_pill_ease(F32 t)
    {
        t = llclamp(t, 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // Profile pictures of avatars with a tag, asked for once per session, a couple a second
    class FSNameTagPortraits : public LLAvatarPropertiesObserver
    {
    public:
        static FSNameTagPortraits& instance()
        {   // never deleted: the observer would unregister from a properties processor already gone at exit
            static FSNameTagPortraits* sInstance = new FSNameTagPortraits();
            return *sInstance;
        }

        // The picture to draw, null while not known or loaded. has_picture: the avatar has one (room is
        // made for it while it loads).
        LLViewerFetchedTexture* get(const LLUUID& avatar_id, bool& has_picture)
        {
            Entry& entry = mEntries[avatar_id];
            if (!entry.mKnown)
            {
                // asked for and no answer (the request failed, or the region had no profile capability yet):
                // ask again a little later, a few times
                if (entry.mQueued && entry.mRequested && entry.mTries < MAX_TRIES &&
                    LLFrameTimer::getElapsedSeconds() - entry.mRequestTime > RETRY_AFTER)
                {
                    entry.mQueued = false;
                }
                if (!entry.mQueued)
                {
                    entry.mQueued = true;
                    entry.mRequested = false;
                    mQueue.push_back(avatar_id);
                }
                has_picture = false;
                return nullptr;
            }
            has_picture = entry.mImage.notNull();
            if (!has_picture)
            {
                return nullptr;
            }
            if (entry.mTexture.isNull())
            {
                // At full resolution, kept as long as the session: a UI texture, which the discard bias
                // doesn't shrink and nothing scales down. As a scene texture (LOD_TEXTURE, no boost) on no
                // object, the priority pass set its wanted size back to nothing whenever the bias was above
                // 1.5 (LLViewerTextureList::updateImageDecodePriority), and it often never loaded. As an
                // icon (BOOST_ICON) it was scaled to the draw size, and shared with the UI's avatar icons,
                // which set theirs as small as 32 pixels.
                entry.mTexture = LLViewerTextureManager::getFetchedTexture(entry.mImage, FTT_DEFAULT, true,
                    LLGLTexture::BOOST_UI, LLViewerTexture::FETCHED_TEXTURE);
            }
            if (entry.mTexture->isMissingAsset())
            {
                has_picture = false;
                return nullptr;
            }
            entry.mTexture->fsRequestNameTagFullResolution(); // <FS:Trish/> also applies to already-cached textures
            return entry.mTexture->getDiscardLevel() >= 0 ? entry.mTexture.get() : nullptr;
        }

        void idle()
        {
            constexpr F32 REQUEST_INTERVAL = 0.5f;
            if (mQueue.empty() || mRequestTimer.getElapsedTimeF32() < REQUEST_INTERVAL)
            {
                return;
            }
            mRequestTimer.reset();
            const LLUUID avatar_id = mQueue.front();
            mQueue.pop_front();
            Entry& entry = mEntries[avatar_id];
            if (entry.mKnown)
            {
                return;
            }
            entry.mRequested = true;
            entry.mRequestTime = LLFrameTimer::getElapsedSeconds();
            ++entry.mTries;
            LLAvatarPropertiesProcessor::getInstance()->addObserver(avatar_id, this);
            LLAvatarPropertiesProcessor::getInstance()->sendAvatarPropertiesRequest(avatar_id);
        }

        void processProperties(void* data, EAvatarProcessorType type) override
        {
            LLUUID avatar_id;
            LLUUID image_id;
            if (type == APT_PROPERTIES && data)
            {
                const LLAvatarData* props = static_cast<const LLAvatarData*>(data);
                avatar_id = props->avatar_id;
                image_id = props->image_id;
            }
            else if (type == APT_PROPERTIES_LEGACY && data)
            {
                const LLAvatarLegacyData* props = static_cast<const LLAvatarLegacyData*>(data);
                avatar_id = props->avatar_id;
                image_id = props->image_id;
            }
            else
            {
                return;
            }
            Entry& entry = mEntries[avatar_id];
            if (!entry.mKnown || entry.mImage != image_id)
            {
                entry.mTexture = nullptr;
            }
            entry.mImage = image_id;
            entry.mKnown = true;
            LLAvatarPropertiesProcessor::getInstance()->removeObserver(avatar_id, this);
        }

    private:
        struct Entry
        {
            LLUUID  mImage;
            bool    mKnown = false;
            bool    mQueued = false;
            bool    mRequested = false;
            S32     mTries = 0;
            F64     mRequestTime = 0.0;
            LLPointer<LLViewerFetchedTexture> mTexture;
        };
        static constexpr S32 MAX_TRIES = 3;
        static constexpr F64 RETRY_AFTER = 20.0;
        std::map<LLUUID, Entry> mEntries;
        std::deque<LLUUID>      mQueue;
        LLFrameTimer            mRequestTimer;
    };

    bool fs_pill_style()
    {
        static LLCachedControl<bool> pill(gSavedSettings, "FSNameTagPillStyle", true);
        return pill;
    }

    bool fs_pill_pictures()
    {
        static LLCachedControl<bool> pictures(gSavedSettings, "FSNameTagProfilePictures", true);
        return pictures;
    }

    // Beyond this distance (m) a tag with a profile picture shows only the picture; 0 never
    F32 fs_pill_picture_only_distance()
    {
        static LLCachedControl<F32> distance(gSavedSettings, "FSNameTagPictureOnlyDistance", 3.f);
        return distance;
    }

    bool fs_pill_ring()
    {
        static LLCachedControl<bool> ring(gSavedSettings, "FSNameTagChatRangeRing", true);
        return ring;
    }

    // Size of a pill tag this far from the camera: full size close by, then shrinking with the square root of the
    // distance, half way between a fixed size and the perspective of the world (0.71 at 10 m, 0.5 at 20 m), down
    // to FSNameTagDistanceMinScale. A straight ramp from 8 to 40 m left most of a crowd at 80 to 100 percent.
    F32 fs_pill_scale(F32 distance)
    {
        static LLCachedControl<bool> scale_with_distance(gSavedSettings, "FSNameTagScaleWithDistance", true);
        static LLCachedControl<F32> min_scale(gSavedSettings, "FSNameTagDistanceMinScale", 0.5f);
        if (!scale_with_distance || distance <= FS_PILL_FULL_SIZE_DIST)
        {
            return 1.f;
        }
        return llclamp(sqrtf(FS_PILL_FULL_SIZE_DIST / distance), llclamp((F32)min_scale, 0.3f, 1.f), 1.f);
    }

    // The tag's scale for a distance, snapped to steps of FS_PILL_SCALE_STEP: the text of a shrunk tag is drawn with
    // a font rasterised at that size (see FSPillFont), and a few sizes keep the number of fonts small. It changes
    // only once the wanted scale is well past the half way point to the next step, so the tag doesn't flip between
    // two sizes at one distance.
    F32 fs_pill_step_scale(F32 distance, F32 current)
    {
        const F32 wanted = fs_pill_scale(distance);
        if (wanted >= 1.f)
        {
            return 1.f;
        }
        if (fabsf(wanted - current) <= 0.5f * FS_PILL_SCALE_STEP + FS_PILL_SCALE_HYSTERESIS)
        {
            return current;
        }
        return llclamp((F32)ll_round(wanted / FS_PILL_SCALE_STEP) * FS_PILL_SCALE_STEP, FS_PILL_SCALE_STEP, 1.f);
    }

    // The font a line of a shrunk tag is drawn with. Scaling the glyphs of the full size font minified them from the
    // font's texture (no mipmaps) into blurry, broken text; instead, the same font (same files, fallbacks for other
    // scripts and emoji, style) rasterised at the smaller size, a derived font size of the registry ("Small@70").
    // It keeps the UI scale and the font size adjustment, as both are part of the named size and of the font's DPI.
    // Made once per font and step, on first use; the fonts belong to the registry, alive to the end of the session.
    // Should one fail to load, the full size font with its glyphs scaled, as before.
    struct FSPillFont
    {
        const LLFontGL* mFont;
        F32 mGlyphScale;    // 1, unless no smaller font could be made

        FSPillFont(const LLFontGL* base, F32 scale)
        :   mFont(base),
            mGlyphScale(1.f)
        {
            const S32 percent = ll_round(scale * 100.f);
            if (!base || percent >= 100)
            {
                return;
            }
            static std::map<std::pair<const LLFontGL*, S32>, const LLFontGL*> fonts;
            auto found = fonts.find(std::make_pair(base, percent));
            if (found == fonts.end())
            {
                const LLFontDescriptor& base_desc = base->getFontDesc();
                LLFontDescriptor desc = base_desc.normalize();
                desc.setTabnum(base_desc.isTabnum());
                desc.setSize(llformat("%s@%d", desc.getSize().c_str(), percent));
                const LLFontGL* font = LLFontGL::getFont(desc);
                if (!font)
                {
                    LL_WARNS_ONCE("NameTags") << "No " << desc.getName() << " font of size " << desc.getSize()
                                              << " for a shrunk name tag; its glyphs are scaled instead" << LL_ENDL;
                }
                found = fonts.emplace(std::make_pair(base, percent), font).first;
            }
            if (found->second)
            {
                mFont = found->second;
            }
            else
            {
                mGlyphScale = scale;
            }
        }

        // in the tag's own (unscaled) pixels
        F32 lineHeight(F32 scale) const
        {
            return (F32)mFont->getLineHeight() * mGlyphScale / scale;
        }
    };

    // hud_render_text at a pixel position: x_vec and y_vec are the tag's pixel vectors, already scaled, and the
    // offsets are in the tag's own (unscaled) pixels
    void fs_pill_text(const LLWString& wstr, const LLVector3& pos_agent, const LLVector3& x_vec, const LLVector3& y_vec,
                      const FSPillFont& pill_font, U8 style, F32 x_offset, F32 y_offset, const LLColor4& color)
    {
        if (wstr.empty())
        {
            return;
        }
        const LLVector3 render_pos = pos_agent + x_vec * x_offset + y_vec * y_offset;
        const LLRect world_view_rect = gViewerWindow->getWorldViewRectRaw();
        const glm::ivec4 viewport(world_view_rect.mLeft, world_view_rect.mBottom, world_view_rect.getWidth(), world_view_rect.getHeight());
        glm::vec3 win_coord = glm::project(glm::vec3(render_pos), get_current_modelview(), get_current_projection(), viewport);

        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.pushMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();
        LLUI::pushMatrix();

        gl_state_for_2d(world_view_rect.getWidth(), world_view_rect.getHeight());
        gViewerWindow->setup3DViewport();

        win_coord.x -= world_view_rect.mLeft;
        win_coord.y -= world_view_rect.mBottom;
        LLUI::loadIdentity();
        gGL.loadIdentity();
        // in raw pixels (the font applies the UI scale itself); glyphs scaled only if no smaller font could be made
        gGL.translatef(floorf(win_coord.x + 0.5f), floorf(win_coord.y + 0.5f), -((win_coord.z * 2.f) - 1.f));
        if (pill_font.mGlyphScale != 1.f)
        {
            gGL.scalef(pill_font.mGlyphScale, pill_font.mGlyphScale, 1.f);
        }
        F32 right_x;
        pill_font.mFont->render(wstr, 0, 0, 0, color, LLFontGL::LEFT, LLFontGL::BASELINE, style, LLFontGL::NO_SHADOW,
                    static_cast<S32>(wstr.length()), 1000, &right_x, /*use_ellipses*/false, /*use_color*/true);

        LLUI::popMatrix();
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
    }

    // A point of the tag's plane, x and y in screen pixels from its anchor
    inline void fs_pill_vertex(const LLVector3& origin, const LLVector3& x_vec, const LLVector3& y_vec, F32 x, F32 y)
    {
        gGL.vertex3fv((origin + x_vec * x + y_vec * y).mV);
    }

    // A rounded rectangle, corners of the given radius (half the height makes a pill), untextured
    void fs_rounded_rect(const LLVector3& origin, const LLVector3& x_vec, const LLVector3& y_vec,
                         F32 left, F32 bottom, F32 right, F32 top, F32 radius, const LLColor4& color)
    {
        constexpr S32 CORNER_SEGMENTS = 8;
        radius = llmax(0.f, llmin(radius, llmin((right - left) * 0.5f, (top - bottom) * 0.5f)));
        const F32 cx = (left + right) * 0.5f;
        const F32 cy = (bottom + top) * 0.5f;
        const F32 corner_x[4] = { right - radius, left + radius, left + radius, right - radius };
        const F32 corner_y[4] = { top - radius, top - radius, bottom + radius, bottom + radius };
        std::vector<LLVector2> outline;
        outline.reserve(4 * (CORNER_SEGMENTS + 1));
        for (S32 corner = 0; corner < 4; ++corner)
        {
            for (S32 i = 0; i <= CORNER_SEGMENTS; ++i)
            {
                const F32 angle = F_PI_BY_TWO * ((F32)corner + (F32)i / (F32)CORNER_SEGMENTS);
                outline.emplace_back(corner_x[corner] + radius * cosf(angle), corner_y[corner] + radius * sinf(angle));
            }
        }
        gGL.getTexUnit(0)->bind(LLViewerFetchedTexture::sWhiteImagep);
        gGL.color4fv(color.mV);
        gGL.begin(LLRender::TRIANGLES);
        for (size_t i = 0; i < outline.size(); ++i)
        {
            const LLVector2& a = outline[i];
            const LLVector2& b = outline[(i + 1) % outline.size()];
            gGL.texCoord2f(0.5f, 0.5f);
            fs_pill_vertex(origin, x_vec, y_vec, cx, cy);
            fs_pill_vertex(origin, x_vec, y_vec, a.mV[VX], a.mV[VY]);
            fs_pill_vertex(origin, x_vec, y_vec, b.mV[VX], b.mV[VY]);
        }
        gGL.end();
    }

    // A disc; textured (the bound texture, centre cropped to a square) or plain white
    void fs_disc(const LLVector3& origin, const LLVector3& x_vec, const LLVector3& y_vec,
                 F32 cx, F32 cy, F32 radius, const LLColor4& color, LLViewerTexture* texture)
    {
        constexpr S32 SEGMENTS = 32;
        F32 su = 0.5f;
        F32 sv = 0.5f;
        if (texture)
        {
            gGL.getTexUnit(0)->bind(texture);
            const F32 w = (F32)llmax(texture->getFullWidth(), 1);
            const F32 h = (F32)llmax(texture->getFullHeight(), 1);
            if (w > h)
            {
                su *= h / w;
            }
            else
            {
                sv *= w / h;
            }
        }
        else
        {
            gGL.getTexUnit(0)->bind(LLViewerFetchedTexture::sWhiteImagep);
        }
        gGL.color4fv(color.mV);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < SEGMENTS; ++i)
        {
            const F32 a0 = F_TWO_PI * (F32)i / (F32)SEGMENTS;
            const F32 a1 = F_TWO_PI * (F32)(i + 1) / (F32)SEGMENTS;
            gGL.texCoord2f(0.5f, 0.5f);
            fs_pill_vertex(origin, x_vec, y_vec, cx, cy);
            gGL.texCoord2f(0.5f + su * cosf(a0), 0.5f + sv * sinf(a0));
            fs_pill_vertex(origin, x_vec, y_vec, cx + radius * cosf(a0), cy + radius * sinf(a0));
            gGL.texCoord2f(0.5f + su * cosf(a1), 0.5f + sv * sinf(a1));
            fs_pill_vertex(origin, x_vec, y_vec, cx + radius * cosf(a1), cy + radius * sinf(a1));
        }
        gGL.end();
    }
}

void LLHUDNameTag::setFSPill(const LLUUID& avatar_id, bool has_ring, const LLColor4& ring_color)
{
    mFSPill = true;
    mFSAvatarID = avatar_id;
    mFSHasRing = has_ring;
    mFSRingColor = ring_color;
}

// Chat bubbles keep the stock look: their label sits on top of the text
bool LLHUDNameTag::fsPillMode() const
{
    return mFSPill && mLabelSegments.empty() && fs_pill_style();
}

void LLHUDNameTag::fsRenderPill(const LLVector3& render_position, const LLVector3& x_pixel_vec_in, const LLVector3& y_pixel_vec_in,
                                const LLColor4& bg_color, F32 alpha_factor)
{
    // mWidth, mHeight and mOffsetY include the distance scale; below, everything is in the tag's own pixels
    // and the pixel vectors are scaled instead
    const F32 scale = llmax(mFSScale, 0.01f);
    const LLVector3 x_pixel_vec = x_pixel_vec_in * scale;
    const LLVector3 y_pixel_vec = y_pixel_vec_in * scale;
    const F32 width = mWidth / scale;
    const F32 height = mHeight / scale;
    const F32 left = -0.5f * width;
    const F32 right = 0.5f * width;
    const F32 top = (F32)mOffsetY / scale;
    const F32 bottom = top - height;
    const F32 mid_y = (top + bottom) * 0.5f;

    // Far away, a tag with a picture shrinks into it (mFSShrink): the box is already part way between the whole
    // tag and the picture on its own, and the picture moves and grows from the tag's round end to the middle
    const F32 shrink = mFSPortrait ? fs_pill_ease(mFSShrink) : 0.f;
    F32 disc_cx = left + height * 0.5f;
    F32 disc_cy = mid_y;
    F32 disc_r = height * 0.5f;
    F32 text_top = top;
    if (shrink > 0.f)
    {
        const F32 above = (mVertAlignment == ALIGN_VERT_CENTER) ? 0.5f : 1.f;   // part of the box above its anchor
        const F32 anchor = top - above * height;
        const F32 full_top = anchor + above * mFSFullHeight;
        const F32 full_r = mFSFullHeight * 0.5f;
        const F32 solo_r = mFSSoloSize * 0.5f;
        const F32 solo_cy = anchor + above * mFSSoloSize - solo_r;
        disc_cx = lerp(-0.5f * mFSFullWidth + full_r, 0.f, shrink);
        disc_cy = lerp(full_top - full_r, solo_cy, shrink);
        disc_r = lerp(full_r, solo_r, shrink);
        text_top = full_top;
    }

    // the background fades as it shrinks, the text faster still
    if (shrink < 1.f)
    {
        LLColor4 bg = bg_color;
        bg.mV[VALPHA] *= 1.f - shrink;
        fs_rounded_rect(render_position, x_pixel_vec, y_pixel_vec, left, bottom, right, top, height * 0.5f, bg);
    }
    const F32 text_fade = llclamp(1.f - 2.f * shrink, 0.f, 1.f);

    // the picture (or the range dot) on the left, then the text after it
    F32 text_left = left + height * 0.5f;
    LLColor4 ring = mFSHasRing && fs_pill_ring() ? mFSRingColor : LLColor4(0.85f, 0.85f, 0.85f, 1.f);
    ring.mV[VALPHA] *= alpha_factor;
    if (mFSPortrait)
    {   // the picture fills the pill's round end, edge to edge
        bool has_picture = false;
        LLViewerFetchedTexture* picture = FSNameTagPortraits::instance().get(mFSAvatarID, has_picture);
        LLColor4 inner = picture ? LLColor4(1.f, 1.f, 1.f, alpha_factor) : LLColor4(0.2f, 0.2f, 0.2f, alpha_factor);
        fs_disc(render_position, x_pixel_vec, y_pixel_vec, disc_cx, disc_cy, disc_r, inner, picture);
        text_left = disc_cx + disc_r + FS_PILL_GAP;
    }
    else if (mFSHasRing && fs_pill_ring())
    {
        const F32 cx = left + height * 0.5f;
        fs_disc(render_position, x_pixel_vec, y_pixel_vec, cx, mid_y, FS_PILL_DOT * 0.5f, ring, nullptr);
        text_left = cx + FS_PILL_DOT * 0.5f + FS_PILL_GAP;
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.getTexUnit(0)->enable(LLTexUnit::TT_TEXTURE);

    const bool centered = !mFSPortrait && !(mFSHasRing && fs_pill_ring()) && mTextAlignment == ALIGN_TEXT_CENTER;
    S32 max_lines = getMaxLines();
    S32 start_segment = max_lines < 0 ? 0 : llmax((S32)0, (S32)mTextSegments.size() - max_lines);
    if (text_fade <= 0.f)
    {
        start_segment = (S32)mTextSegments.size();
    }
    F32 y_offset = text_top - FS_PILL_PAD_TOP;
    for (auto segment_iter = mTextSegments.begin() + start_segment; segment_iter != mTextSegments.end(); ++segment_iter)
    {
        const FSPillFont pill_font(segment_iter->mFont, scale);
        y_offset -= pill_font.lineHeight(scale);
        const F32 x_offset = centered ? -0.5f * segment_iter->getWidth(pill_font.mFont) * pill_font.mGlyphScale / scale : text_left;
        LLColor4 text_color = segment_iter->mColor;
        text_color.mV[VALPHA] *= alpha_factor * text_fade;
        fs_pill_text(segment_iter->getText(), render_position, x_pixel_vec, y_pixel_vec, pill_font, segment_iter->mStyle,
                     x_offset, y_offset, text_color);
        y_offset -= FS_PILL_LINE_PAD;
    }
    gGL.color4f(1.0f, 1.0f, 1.0f, 1.0f);
}
// </FS:Trish>

bool llhudnametag_further_away::operator()(const LLPointer<LLHUDNameTag>& lhs, const LLPointer<LLHUDNameTag>& rhs) const
{
    return lhs->getDistance() > rhs->getDistance();
}


LLHUDNameTag::LLHUDNameTag(const U8 type)
:   LLHUDObject(type),
    mDoFade(true),
    mFadeDistance(8.f),
    mFadeRange(4.f),
    mLastDistance(0.f),
    mZCompare(true),
    mVisibleOffScreen(false),
    mOffscreen(false),
    mColor(1.f, 1.f, 1.f, 1.f),
//  mScale(),
    mWidth(0.f),
    mHeight(0.f),
    mFontp(LLFontGL::getFontSansSerifSmall()),
    mBoldFontp(LLFontGL::getFontSansSerifBold()),
    mSoftScreenRect(),
    mPositionAgent(),
    mPositionOffset(),
    mMass(10.f),
    mMaxLines(10),
    mOffsetY(0),
    mRadius(0.1f),
    mTextSegments(),
    mLabelSegments(),
    mTextAlignment(ALIGN_TEXT_CENTER),
    mVertAlignment(ALIGN_VERT_CENTER),
    mLOD(0),
    mHidden(false)
{
    LLPointer<LLHUDNameTag> ptr(this);
    sTextObjects.insert(ptr);

    mRoundedRectImgp = LLUI::getUIImage("Rounded_Rect");
    mRoundedRectTopImgp = LLUI::getUIImage("Rounded_Rect_Top");
}

LLHUDNameTag::~LLHUDNameTag()
{
}


bool LLHUDNameTag::lineSegmentIntersect(const LLVector4a& start, const LLVector4a& end, LLVector4a& intersection, bool debug_render)
{
    if (!mVisible || mHidden)
    {
        return false;
    }

    // don't pick text that isn't bound to a viewerobject
    if (!mSourceObject || mSourceObject->mDrawable.isNull())
    {
        return false;
    }

    F32 alpha_factor = 1.f;
    LLColor4 text_color = mColor;
    if (mDoFade)
    {
        if (mLastDistance > mFadeDistance)
        {
            alpha_factor = llmax(0.f, 1.f - (mLastDistance - mFadeDistance)/mFadeRange);
            text_color.mV[3] = text_color.mV[3]*alpha_factor;
        }
    }
    if (text_color.mV[3] < 0.01f)
    {
        return false;
    }

    mOffsetY = lltrunc(mHeight * ((mVertAlignment == ALIGN_VERT_CENTER) ? 0.5f : 1.f));

    LLVector3 position = mPositionAgent;

    if (mSourceObject)
    { //get intersection of eye through mPositionAgent to plane of source object
        //using this position keeps the camera from focusing on some seemingly random
        //point several meters in front of the nametag
        const LLVector3& p = mSourceObject->getPositionAgent();
        const LLVector3& n = LLViewerCamera::getInstance()->getAtAxis();
        const LLVector3& eye = LLViewerCamera::getInstance()->getOrigin();

        LLVector3 ray = position-eye;
        ray.normalize();

        LLVector3 delta = p-position;
        F32 dist = delta*n;
        F32 dt =  dist/(ray*n);
        position += ray*dt;
    }

    // scale screen size of borders down

    LLVector3 x_pixel_vec;
    LLVector3 y_pixel_vec;

    LLViewerCamera::getInstance()->getPixelVectors(position, y_pixel_vec, x_pixel_vec);

    LLVector3 width_vec = mWidth * x_pixel_vec;
    LLVector3 height_vec = mHeight * y_pixel_vec;

    LLCoordGL screen_pos;
    LLViewerCamera::getInstance()->projectPosAgentToScreen(position, screen_pos, false);

    LLVector2 screen_offset;
    screen_offset = updateScreenPos(mPositionOffset);

    LLVector3 render_position = position
            + (x_pixel_vec * screen_offset.mV[VX])
            + (y_pixel_vec * screen_offset.mV[VY]);


    LLVector3 bg_pos = render_position
        + (F32)mOffsetY * y_pixel_vec
        - (width_vec / 2.f)
        - (height_vec);

    LLVector3 v[] =
    {
        bg_pos,
        bg_pos + width_vec,
        bg_pos + width_vec + height_vec,
        bg_pos + height_vec,
    };

    LLVector4a dir;
    dir.setSub(end,start);
    F32 a, b, t;

    LLVector4a v0,v1,v2,v3;
    v0.load3(v[0].mV);
    v1.load3(v[1].mV);
    v2.load3(v[2].mV);
    v3.load3(v[3].mV);

    if (LLTriangleRayIntersect(v0, v1, v2, start, dir, a, b, t) ||
        LLTriangleRayIntersect(v2, v3, v0, start, dir, a, b, t) )
    {
        if (t <= 1.f)
        {
            dir.mul(t);
            intersection.setAdd(start, dir);
            return true;
        }
    }

    return false;
}

void LLHUDNameTag::render()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;
    if (sDisplayText)
    {
        LLGLDepthTest gls_depth(GL_TRUE, GL_FALSE);
        renderText();
    }
}

void LLHUDNameTag::renderText()
{
    if (!mVisible || mHidden)
    {
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;

    gGL.getTexUnit(0)->enable(LLTexUnit::TT_TEXTURE);

    LLColor4 shadow_color(0.f, 0.f, 0.f, 1.f);
    F32 alpha_factor = 1.f;
    LLColor4 text_color = mColor;
    if (mDoFade)
    {
        if (mLastDistance > mFadeDistance)
        {
            alpha_factor = llmax(0.f, 1.f - (mLastDistance - mFadeDistance)/mFadeRange);
            text_color.mV[3] = text_color.mV[3]*alpha_factor;
        }
    }
    if (text_color.mV[3] < 0.01f)
    {
        return;
    }
    shadow_color.mV[3] = text_color.mV[3];

    mOffsetY = lltrunc(mHeight * ((mVertAlignment == ALIGN_VERT_CENTER) ? 0.5f : 1.f));

    // *TODO: make this a per-text setting
    static LLCachedControl<F32> bubble_opacity(gSavedSettings, "ChatBubbleOpacity");
    static LLUIColor nametag_bg_color = LLUIColorTable::instance().getColor("NameTagBackground");
    F32 color_alpha = bubble_opacity * alpha_factor;
    LLColor4 bg_color = nametag_bg_color;
    bg_color.setAlpha(color_alpha);

    // scale screen size of borders down
    //RN: for now, text on hud objects is never occluded

    LLVector3 x_pixel_vec;
    LLVector3 y_pixel_vec;

    LLViewerCamera::getInstance()->getPixelVectors(mPositionAgent, y_pixel_vec, x_pixel_vec);

    LLVector3 width_vec = mWidth * x_pixel_vec;
    LLVector3 height_vec = mHeight * y_pixel_vec;

    mRadius = (width_vec + height_vec).magVec() * 0.5f;

    LLCoordGL screen_pos;
    LLViewerCamera::getInstance()->projectPosAgentToScreen(mPositionAgent, screen_pos, false);

    LLVector2 screen_offset = updateScreenPos(mPositionOffset);

    LLVector3 render_position = mPositionAgent
            + (x_pixel_vec * screen_offset.mV[VX])
            + (y_pixel_vec * screen_offset.mV[VY]);

    // Check if an underwater name tag should be rendered over the water (while camera is above the water)
    bool render_over_water = false;
    static LLCachedControl<bool> nametag_over_water(gSavedSettings, "NametagOverWater", true);
    if (nametag_over_water &&
        mSourceObject &&
        mSourceObject->getRegion() &&
        LLPipeline::sRenderTransparentWater &&
        !LLViewerCamera::getInstance()->cameraUnderWater())
    {
        if (mSourceObject->getPositionAgent().mV[VZ] < mSourceObject->getRegion()->getWaterHeight())
        {
            render_over_water = true;
        }
    }
    LLGLDepthTest gls_depth(GL_TRUE, GL_FALSE, render_over_water ? GL_ALWAYS : GL_LEQUAL);

    // <FS:Trish> Pill style
    if (fsPillMode())
    {
        fsRenderPill(render_position, x_pixel_vec, y_pixel_vec, bg_color, alpha_factor);
        return;
    }
    // </FS:Trish>

    LLRect screen_rect;
    screen_rect.setCenterAndSize(0, static_cast<S32>(lltrunc(-mHeight / 2 + mOffsetY)), static_cast<S32>(lltrunc(mWidth)), static_cast<S32>(lltrunc(mHeight)));
    mRoundedRectImgp->draw3D(render_position, x_pixel_vec, y_pixel_vec, screen_rect, bg_color);
    if (mLabelSegments.size())
    {
        LLRect label_top_rect = screen_rect;
        const S32 label_height = ll_round((mFontp->getLineHeight() * (F32)mLabelSegments.size() + (VERTICAL_PADDING / 3.f)));
        label_top_rect.mBottom = label_top_rect.mTop - label_height;
        LLColor4 label_top_color = text_color;
        // <FS:Ansariel> Performance improvement
        //label_top_color.mV[VALPHA] = bubble_opacity() * alpha_factor;
        label_top_color.mV[VALPHA] = color_alpha;
        // </FS:Ansariel>

        mRoundedRectTopImgp->draw3D(render_position, x_pixel_vec, y_pixel_vec, label_top_rect, label_top_color);
    }

    F32 y_offset = (F32)mOffsetY;

    // Render label
    {
        for(std::vector<LLHUDTextSegment>::iterator segment_iter = mLabelSegments.begin();
            segment_iter != mLabelSegments.end(); ++segment_iter )
        {
            // Label segments use default font
            const LLFontGL* fontp = (segment_iter->mStyle == LLFontGL::BOLD) ? mBoldFontp : mFontp;
            y_offset -= fontp->getLineHeight();

            F32 x_offset;
            if (mTextAlignment == ALIGN_TEXT_CENTER)
            {
                x_offset = -0.5f*segment_iter->getWidth(fontp);
            }
            else // ALIGN_LEFT
            {
                x_offset = -0.5f * mWidth + (HORIZONTAL_PADDING / 2.f);
            }

            LLColor4 label_color(0.f, 0.f, 0.f, alpha_factor);
            hud_render_text(segment_iter->getText(), render_position, *fontp, segment_iter->mStyle, LLFontGL::NO_SHADOW, x_offset, y_offset, label_color, false);
        }
    }

    // Render text
    {
        // -1 mMaxLines means unlimited lines.
        S32 start_segment;
        S32 max_lines = getMaxLines();

        if (max_lines < 0)
        {
            start_segment = 0;
        }
        else
        {
            start_segment = llmax((S32)0, (S32)mTextSegments.size() - max_lines);
        }

        for (std::vector<LLHUDTextSegment>::iterator segment_iter = mTextSegments.begin() + start_segment;
             segment_iter != mTextSegments.end(); ++segment_iter )
        {
            const LLFontGL* fontp = segment_iter->mFont;
            y_offset -= fontp->getLineHeight();
            y_offset -= LINE_PADDING;

            U8 style = segment_iter->mStyle;
            LLFontGL::ShadowType shadow = LLFontGL::DROP_SHADOW;

            F32 x_offset;
            if (mTextAlignment== ALIGN_TEXT_CENTER)
            {
                x_offset = -0.5f*segment_iter->getWidth(fontp);
            }
            else // ALIGN_LEFT
            {
                x_offset = -0.5f * mWidth + (HORIZONTAL_PADDING / 2.f);

                // *HACK
                x_offset += 1;
            }

            text_color = segment_iter->mColor;
            text_color.mV[VALPHA] *= alpha_factor;

            hud_render_text(segment_iter->getText(), render_position, *fontp, style, shadow, x_offset, y_offset, text_color, false);
        }
    }
    /// Reset the default color to white.  The renderer expects this to be the default.
    gGL.color4f(1.0f, 1.0f, 1.0f, 1.0f);
}

void LLHUDNameTag::setString(const std::string &text_utf8)
{
    mTextSegments.clear();
    addLine(text_utf8, mColor);
}

void LLHUDNameTag::clearString()
{
    mTextSegments.clear();
}


void LLHUDNameTag::addLine(const std::string &text_utf8,
                        const LLColor4& color,
                        const LLFontGL::StyleFlags style,
                        const LLFontGL* font,
                        const bool use_ellipses,
                        F32 max_pixels)
{
    LLWString wline = utf8str_to_wstring(text_utf8);
    if (!wline.empty())
    {
        // use default font for segment if custom font not specified
        if (!font)
        {
            font = mFontp;
        }
        typedef boost::tokenizer<boost::char_separator<llwchar>, LLWString::const_iterator, LLWString > tokenizer;
        LLWString seps(utf8str_to_wstring("\r\n"));
        boost::char_separator<llwchar> sep(seps.c_str());

        tokenizer tokens(wline, sep);
        tokenizer::iterator iter = tokens.begin();

        max_pixels = llmin(max_pixels, NAMETAG_MAX_WIDTH);
        while (iter != tokens.end())
        {
            U32 line_length = 0;
            if (use_ellipses)
            {
                // "QualityAssuranceAssuresQuality1" will end up like "QualityAssuranceAssuresQual..."
                // "QualityAssuranceAssuresQuality QualityAssuranceAssuresQuality" will end up like "QualityAssuranceAssuresQual..."
                // "QualityAssurance AssuresQuality1" will end up as "QualityAssurance AssuresQua..." because we are enforcing single line
                do
                {
                    auto segment_length = font->maxDrawableChars(iter->substr(line_length).c_str(), max_pixels, static_cast<S32>(wline.length()), LLFontGL::ANYWHERE);
                    if (segment_length + line_length < wline.length()) // since we only draw one string, line_length should be 0
                    {
                        // token does does not fit into signle line, need to draw "...".
                        // Use four dots for ellipsis width to generate padding
                        const LLWString dots_pad(utf8str_to_wstring(std::string("....")));
                        S32 elipses_width = (S32)font->getWidthF32(dots_pad.c_str());
                        // truncated string length
                        segment_length = font->maxDrawableChars(iter->substr(line_length).c_str(), max_pixels - elipses_width, static_cast<S32>(wline.length()), LLFontGL::ANYWHERE);
                        const LLWString dots(utf8str_to_wstring(std::string("...")));
                        LLHUDTextSegment segment(iter->substr(line_length, segment_length) + dots, style, color, font);
                        mTextSegments.push_back(segment);
                        break; // consider it to be complete
                    }
                    else
                    {
                        // token fits fully into string
                        LLHUDTextSegment segment(iter->substr(line_length, segment_length), style, color, font);
                        mTextSegments.push_back(segment);
                        line_length += segment_length;
                    }
                } while (line_length != iter->size());
            }
            else
            {
                // "QualityAssuranceAssuresQuality 1" will be split into two lines "QualityAssuranceAssuresQualit" and "y 1"
                // "QualityAssurance AssuresQuality 1" will be split into two lines "QualityAssurance" and "AssuresQuality"
                do
                {
                    S32 segment_length = font->maxDrawableChars(iter->substr(line_length).c_str(), max_pixels, static_cast<S32>(wline.length()), LLFontGL::WORD_BOUNDARY_IF_POSSIBLE);
                    LLHUDTextSegment segment(iter->substr(line_length, segment_length), style, color, font);
                    mTextSegments.push_back(segment);
                    line_length += segment_length;
                } while (line_length != iter->size());
            }
            ++iter;
        }
    }
}

void LLHUDNameTag::setLabel(const std::string &label_utf8)
{
    mLabelSegments.clear();
    addLabel(label_utf8);
}

void LLHUDNameTag::addLabel(const std::string& label_utf8, F32 max_pixels)
{
    LLWString wstr = utf8string_to_wstring(label_utf8);
    if (!wstr.empty())
    {
        LLWString seps(utf8str_to_wstring("\r\n"));
        LLWString empty;

        typedef boost::tokenizer<boost::char_separator<llwchar>, LLWString::const_iterator, LLWString > tokenizer;
        boost::char_separator<llwchar> sep(seps.c_str(), empty.c_str(), boost::keep_empty_tokens);

        tokenizer tokens(wstr, sep);
        tokenizer::iterator iter = tokens.begin();

        max_pixels = llmin(max_pixels, NAMETAG_MAX_WIDTH);

        while (iter != tokens.end())
        {
            U32 line_length = 0;
            do
            {
                S32 segment_length = mFontp->maxDrawableChars(iter->substr(line_length).c_str(),
                    max_pixels, static_cast<S32>(wstr.length()), LLFontGL::WORD_BOUNDARY_IF_POSSIBLE);
                LLHUDTextSegment segment(iter->substr(line_length, segment_length), LLFontGL::NORMAL, mColor, mFontp);
                mLabelSegments.push_back(segment);
                line_length += segment_length;
            }
            while (line_length != iter->size());
            ++iter;
        }
    }
}

void LLHUDNameTag::setZCompare(const bool zcompare)
{
    mZCompare = zcompare;
}

void LLHUDNameTag::setFont(const LLFontGL* font)
{
    mFontp = font;
}


void LLHUDNameTag::setColor(const LLColor4 &color)
{
    mColor = color;
    for (std::vector<LLHUDTextSegment>::iterator segment_iter = mTextSegments.begin();
         segment_iter != mTextSegments.end(); ++segment_iter )
    {
        segment_iter->mColor = color;
    }
}

void LLHUDNameTag::setAlpha(F32 alpha)
{
    mColor.mV[VALPHA] = alpha;
    for (std::vector<LLHUDTextSegment>::iterator segment_iter = mTextSegments.begin();
         segment_iter != mTextSegments.end(); ++segment_iter )
    {
        segment_iter->mColor.mV[VALPHA] = alpha;
    }
}


void LLHUDNameTag::setDoFade(const bool do_fade)
{
    mDoFade = do_fade;
}

void LLHUDNameTag::updateVisibility()
{
    if (mSourceObject)
    {
        mSourceObject->updateText();
    }

    mPositionAgent = gAgent.getPosAgentFromGlobal(mPositionGlobal);

    if (!mSourceObject)
    {
        //LL_WARNS() << "LLHUDNameTag::updateScreenPos -- mSourceObject is NULL!" << LL_ENDL;
        mVisible = true;
        sVisibleTextObjects.push_back(LLPointer<LLHUDNameTag> (this));
        return;
    }

    // Not visible if parent object is dead
    if (mSourceObject->isDead())
    {
        mVisible = false;
        return;
    }

    // push text towards camera by radius of object, but not past camera
    LLVector3 vec_from_camera = mPositionAgent - LLViewerCamera::getInstance()->getOrigin();
    LLVector3 dir_from_camera = vec_from_camera;
    dir_from_camera.normVec();

    if (dir_from_camera * LLViewerCamera::getInstance()->getAtAxis() <= 0.f)
    { //text is behind camera, don't render
        mVisible = false;
        return;
    }

    if (vec_from_camera * LLViewerCamera::getInstance()->getAtAxis() <= LLViewerCamera::getInstance()->getNear() + 0.1f + mSourceObject->getVObjRadius())
    {
        mPositionAgent = LLViewerCamera::getInstance()->getOrigin() + vec_from_camera * ((LLViewerCamera::getInstance()->getNear() + 0.1f) / (vec_from_camera * LLViewerCamera::getInstance()->getAtAxis()));
    }
    else
    {
        mPositionAgent -= dir_from_camera * mSourceObject->getVObjRadius();
    }

    mLastDistance = (mPositionAgent - LLViewerCamera::getInstance()->getOrigin()).magVec();

    if (mLOD >= 3 || !mTextSegments.size() || (mDoFade && (mLastDistance > mFadeDistance + mFadeRange)))
    {
        mVisible = false;
        return;
    }

    LLVector3 x_pixel_vec;
    LLVector3 y_pixel_vec;

    LLViewerCamera::getInstance()->getPixelVectors(mPositionAgent, y_pixel_vec, x_pixel_vec);

    LLVector3 render_position = mPositionAgent +
            (x_pixel_vec * mPositionOffset.mV[VX]) +
            (y_pixel_vec * mPositionOffset.mV[VY]);

    mOffscreen = false;
    if (!LLViewerCamera::getInstance()->sphereInFrustum(render_position, mRadius))
    {
        if (!mVisibleOffScreen)
        {
            mVisible = false;
            return;
        }
        else
        {
            mOffscreen = true;
        }
    }

    mVisible = true;
    sVisibleTextObjects.push_back(LLPointer<LLHUDNameTag> (this));
}

LLVector2 LLHUDNameTag::updateScreenPos(LLVector2 &offset)
{
    LLCoordGL screen_pos;
    LLVector2 screen_pos_vec;
    LLVector3 x_pixel_vec;
    LLVector3 y_pixel_vec;
    LLViewerCamera::getInstance()->getPixelVectors(mPositionAgent, y_pixel_vec, x_pixel_vec);
    LLVector3 world_pos = mPositionAgent + (offset.mV[VX] * x_pixel_vec) + (offset.mV[VY] * y_pixel_vec);
    if (!LLViewerCamera::getInstance()->projectPosAgentToScreen(world_pos, screen_pos, false) && mVisibleOffScreen)
    {
        // bubble off-screen, so find a spot for it along screen edge
        LLViewerCamera::getInstance()->projectPosAgentToScreenEdge(world_pos, screen_pos);
    }

    screen_pos_vec.setVec((F32)screen_pos.mX, (F32)screen_pos.mY);

    LLRect world_rect = gViewerWindow->getWorldViewRectScaled();
    S32 bottom = world_rect.mBottom + STATUS_BAR_HEIGHT;

    LLVector2 screen_center;
    screen_center.mV[VX] = llclamp((F32)screen_pos_vec.mV[VX], (F32)world_rect.mLeft + mWidth * 0.5f, (F32)world_rect.mRight - mWidth * 0.5f);

    if(mVertAlignment == ALIGN_VERT_TOP)
    {
        screen_center.mV[VY] = llclamp((F32)screen_pos_vec.mV[VY],
            (F32)bottom,
            (F32)world_rect.mTop - mHeight - (F32)MENU_BAR_HEIGHT);
        mSoftScreenRect.setLeftTopAndSize(screen_center.mV[VX] - (mWidth + BUFFER_SIZE) * 0.5f,
            screen_center.mV[VY] + (mHeight + BUFFER_SIZE), mWidth + BUFFER_SIZE, mHeight + BUFFER_SIZE);
    }
    else
    {
        screen_center.mV[VY] = llclamp((F32)screen_pos_vec.mV[VY],
            (F32)bottom + mHeight * 0.5f,
            (F32)world_rect.mTop - mHeight * 0.5f - (F32)MENU_BAR_HEIGHT);
        mSoftScreenRect.setCenterAndSize(screen_center.mV[VX], screen_center.mV[VY], mWidth + BUFFER_SIZE, mHeight + BUFFER_SIZE);
    }

    return offset + (screen_center - LLVector2((F32)screen_pos.mX, (F32)screen_pos.mY));
}

void LLHUDNameTag::updateSize()
{
    F32 height = 0.f;
    F32 width = 0.f;

    S32 max_lines = getMaxLines();
    //S32 lines = (max_lines < 0) ? (S32)mTextSegments.size() : llmin((S32)mTextSegments.size(), max_lines);
    //F32 height = (F32)mFontp->getLineHeight() * (lines + mLabelSegments.size());

    S32 start_segment;
    if (max_lines < 0) start_segment = 0;
    else start_segment = llmax((S32)0, (S32)mTextSegments.size() - max_lines);

    std::vector<LLHUDTextSegment>::iterator iter = mTextSegments.begin() + start_segment;
    while (iter != mTextSegments.end())
    {
        const LLFontGL* fontp = iter->mFont;
        height += fontp->getLineHeight();
        height += LINE_PADDING;
        width = llmax(width, llmin(iter->getWidth(fontp), NAMETAG_MAX_WIDTH));
        ++iter;
    }

    // Don't want line spacing under the last line
    if (height > 0.f)
    {
        height -= LINE_PADDING;
    }

    iter = mLabelSegments.begin();
    while (iter != mLabelSegments.end())
    {
        height += mFontp->getLineHeight();
        width = llmax(width, llmin(iter->getWidth(mFontp), NAMETAG_MAX_WIDTH));
        ++iter;
    }

    if (width == 0.f)
    {
        return;
    }

    // <FS:Trish> Pill style: less padding, round ends, room for the picture or the range dot
    if (fsPillMode())
    {
        // Sized for the fonts the text is drawn with at this distance (FSPillFont), in the tag's own pixels: a
        // smaller font isn't exactly the full size font scaled, and the box must fit the text it holds
        mFSScale = fs_pill_step_scale(mLastDistance, mFSScale);
        S32 lines = 0;
        F32 text_height = 0.f;
        width = 0.f;
        for (auto seg = mTextSegments.begin() + start_segment; seg != mTextSegments.end(); ++seg)
        {
            const FSPillFont pill_font(seg->mFont, mFSScale);
            text_height += pill_font.lineHeight(mFSScale);
            ++lines;
            const F32 full_width = seg->getWidth(seg->mFont);
            F32 line_width = llmin(full_width, NAMETAG_MAX_WIDTH);
            if (pill_font.mFont != seg->mFont && full_width > 0.f)
            {   // the same share of the line, measured with the font it is drawn with
                line_width *= seg->getWidth(pill_font.mFont) * pill_font.mGlyphScale / (mFSScale * full_width);
            }
            width = llmax(width, line_width);
        }
        mHeight = text_height + FS_PILL_LINE_PAD * (F32)llmax(lines - 1, 0) + FS_PILL_PAD_TOP + FS_PILL_PAD_BOTTOM;

        bool has_picture = false;
        if (fs_pill_pictures() && mFSAvatarID.notNull())
        {
            FSNameTagPortraits::instance().get(mFSAvatarID, has_picture);
        }
        mFSPortrait = has_picture && mHeight >= FS_PILL_MIN_PICTURE;
        const F32 end = mHeight * 0.5f;     // a round end's width
        if (mFSPortrait)
        {
            mWidth = mHeight + FS_PILL_GAP + width + end;
        }
        else if (mFSHasRing && fs_pill_ring())
        {
            mWidth = end + FS_PILL_DOT + FS_PILL_GAP + width + end;
        }
        else
        {
            mWidth = width + 2.f * end;
        }

        // far away, a tag with a picture shrinks into the picture: a disc half as high again as a one line tag
        const F32 picture_only_distance = fs_pill_picture_only_distance();
        const bool picture_only = mFSPortrait && picture_only_distance > 0.f && mLastDistance > picture_only_distance &&
                                  start_segment < (S32)mTextSegments.size();
        const F64 now = LLFrameTimer::getElapsedSeconds();
        const F32 elapsed = (F32)(now - mFSShrinkTime);
        mFSShrinkTime = now;
        const F32 target = picture_only ? 1.f : 0.f;
        if (!mFSPortrait || elapsed > 1.f)
        {   // no picture, or a tag not updated for a while (just made): no animation
            mFSShrink = target;
        }
        else
        {
            const F32 step = elapsed / FS_PILL_SHRINK_SECONDS;
            mFSShrink = target > mFSShrink ? llmin(target, mFSShrink + step) : llmax(target, mFSShrink - step);
        }
        if (mFSPortrait && start_segment < (S32)mTextSegments.size())
        {
            mFSFullWidth = mWidth;
            mFSFullHeight = mHeight;
            const FSPillFont first_font(mTextSegments[start_segment].mFont, mFSScale);
            mFSSoloSize = 1.5f * (first_font.lineHeight(mFSScale) + FS_PILL_PAD_TOP + FS_PILL_PAD_BOTTOM);
            const F32 shrink = fs_pill_ease(mFSShrink);
            mWidth = lerp(mFSFullWidth, mFSSoloSize, shrink);
            mHeight = lerp(mFSFullHeight, mFSSoloSize, shrink);
        }
        else
        {
            mFSShrink = 0.f;
        }
        mWidth *= mFSScale;
        mHeight *= mFSScale;
        return;
    }
    mFSPortrait = false;
    mFSShrink = 0.f;
    mFSScale = 1.f;
    // </FS:Trish>

    width += HORIZONTAL_PADDING;
    height += VERTICAL_PADDING;

    // *TODO: Could do a timer-based resize here
    //mWidth = llmax(width, lerp(mWidth, (F32)width, u));
    //mHeight = llmax(height, lerp(mHeight, (F32)height, u));
    mWidth = width;
    mHeight = height;
}

void LLHUDNameTag::updateAll()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;
    // iterate over all text objects, calculate their restoration forces,
    // and add them to the visible set if they are on screen and close enough
    sVisibleTextObjects.clear();

    // <FS:Trish> Pill style name tags: ask for the next profile picture
    if (fs_pill_style() && fs_pill_pictures())
    {
        FSNameTagPortraits::instance().idle();
    }
    // </FS:Trish>

    TextObjectIterator text_it;
    for (text_it = sTextObjects.begin(); text_it != sTextObjects.end(); ++text_it)
    {
        LLHUDNameTag* textp = (*text_it);
        textp->mTargetPositionOffset.clearVec();
        textp->updateSize();
        textp->updateVisibility();
    }

    // sort back to front for rendering purposes
    std::sort(sVisibleTextObjects.begin(), sVisibleTextObjects.end(), llhudnametag_further_away());

    // iterate from front to back, and set LOD based on current screen coverage
    F32 screen_area = (F32)(gViewerWindow->getWindowWidthScaled() * gViewerWindow->getWindowHeightScaled());
    F32 current_screen_area = 0.f;
    std::vector<LLPointer<LLHUDNameTag> >::reverse_iterator r_it;
    for (r_it = sVisibleTextObjects.rbegin(); r_it != sVisibleTextObjects.rend(); ++r_it)
    {
        LLHUDNameTag* textp = (*r_it);
        if (current_screen_area / screen_area > LOD_2_SCREEN_COVERAGE)
        {
            textp->setLOD(3);
        }
        else if (current_screen_area / screen_area > LOD_1_SCREEN_COVERAGE)
        {
            textp->setLOD(2);
        }
        else if (current_screen_area / screen_area > LOD_0_SCREEN_COVERAGE)
        {
            textp->setLOD(1);
        }
        else
        {
            textp->setLOD(0);
        }
        textp->updateSize();
        // find on-screen position and initialize collision rectangle
        textp->mTargetPositionOffset = textp->updateScreenPos(LLVector2::zero);
        current_screen_area += (F32)(textp->mSoftScreenRect.getWidth() * textp->mSoftScreenRect.getHeight());
    }

    LLTrace::CountStatHandle<>* camera_vel_stat = LLViewerCamera::getVelocityStat();
    F32 camera_vel = (F32)LLTrace::get_frame_recording().getLastRecording().getPerSec(*camera_vel_stat);
    if (camera_vel > MAX_STABLE_CAMERA_VELOCITY)
    {
        return;
    }

    VisibleTextObjectIterator src_it;

    for (S32 i = 0; i < NUM_OVERLAP_ITERATIONS; i++)
    {
        bool moved = false; // <FS:Trish/> a pass without overlaps changes nothing, so neither would the rest
        for (src_it = sVisibleTextObjects.begin(); src_it != sVisibleTextObjects.end(); ++src_it)
        {
            LLHUDNameTag* src_textp = (*src_it);

            VisibleTextObjectIterator dst_it = src_it;
            ++dst_it;
            for (; dst_it != sVisibleTextObjects.end(); ++dst_it)
            {
                LLHUDNameTag* dst_textp = (*dst_it);

                if (src_textp->mSoftScreenRect.overlaps(dst_textp->mSoftScreenRect))
                {
                    LLRectf intersect_rect = src_textp->mSoftScreenRect;
                    intersect_rect.intersectWith(dst_textp->mSoftScreenRect);
                    intersect_rect.stretch(-BUFFER_SIZE * 0.5f);

                    F32 src_center_x = src_textp->mSoftScreenRect.getCenterX();
                    F32 src_center_y = src_textp->mSoftScreenRect.getCenterY();
                    F32 dst_center_x = dst_textp->mSoftScreenRect.getCenterX();
                    F32 dst_center_y = dst_textp->mSoftScreenRect.getCenterY();
                    F32 intersect_center_x = intersect_rect.getCenterX();
                    F32 intersect_center_y = intersect_rect.getCenterY();
                    LLVector2 force = lerp(LLVector2(dst_center_x - intersect_center_x, dst_center_y - intersect_center_y),
                                        LLVector2(intersect_center_x - src_center_x, intersect_center_y - src_center_y),
                                        0.5f);
                    force.setVec(dst_center_x - src_center_x, dst_center_y - src_center_y);
                    force.normVec();

                    LLVector2 src_force = -1.f * force;
                    LLVector2 dst_force = force;

                    LLVector2 force_strength;
                    F32 src_mult = dst_textp->mMass / (dst_textp->mMass + src_textp->mMass);
                    F32 dst_mult = 1.f - src_mult;
                    F32 src_aspect_ratio = src_textp->mSoftScreenRect.getWidth() / src_textp->mSoftScreenRect.getHeight();
                    F32 dst_aspect_ratio = dst_textp->mSoftScreenRect.getWidth() / dst_textp->mSoftScreenRect.getHeight();
                    src_force.mV[VY] *= src_aspect_ratio;
                    src_force.normVec();
                    dst_force.mV[VY] *= dst_aspect_ratio;
                    dst_force.normVec();

                    src_force.mV[VX] *= llmin(intersect_rect.getWidth() * src_mult, intersect_rect.getHeight() * SPRING_STRENGTH);
                    src_force.mV[VY] *= llmin(intersect_rect.getHeight() * src_mult, intersect_rect.getWidth() * SPRING_STRENGTH);
                    dst_force.mV[VX] *=  llmin(intersect_rect.getWidth() * dst_mult, intersect_rect.getHeight() * SPRING_STRENGTH);
                    dst_force.mV[VY] *=  llmin(intersect_rect.getHeight() * dst_mult, intersect_rect.getWidth() * SPRING_STRENGTH);

                    src_textp->mTargetPositionOffset += src_force;
                    dst_textp->mTargetPositionOffset += dst_force;
                    src_textp->mTargetPositionOffset = src_textp->updateScreenPos(src_textp->mTargetPositionOffset);
                    dst_textp->mTargetPositionOffset = dst_textp->updateScreenPos(dst_textp->mTargetPositionOffset);
                    moved = true; // <FS:Trish/>
                }
            }
        }
        // <FS:Trish>
        if (!moved)
        {
            break;
        }
        // </FS:Trish>
    }

    VisibleTextObjectIterator this_object_it;
    for (this_object_it = sVisibleTextObjects.begin(); this_object_it != sVisibleTextObjects.end(); ++this_object_it)
    {
        (*this_object_it)->mPositionOffset = lerp((*this_object_it)->mPositionOffset, (*this_object_it)->mTargetPositionOffset, LLSmoothInterpolation::getInterpolant(POSITION_DAMPING_TC));
    }
}

void LLHUDNameTag::setLOD(S32 lod)
{
    mLOD = lod;
    //RN: uncomment this to visualize LOD levels
    //std::string label = llformat("%d", lod);
    //setLabel(label);
}

S32 LLHUDNameTag::getMaxLines()
{
    switch(mLOD)
    {
    case 0:
        return mMaxLines;
    case 1:
        return mMaxLines > 0 ? mMaxLines / 2 : 5;
    case 2:
        return mMaxLines > 0 ? mMaxLines / 3 : 2;
    default:
        // label only
        return 0;
    }
}

void LLHUDNameTag::markDead()
{
    sTextObjects.erase(LLPointer<LLHUDNameTag>(this));
    LLHUDObject::markDead();
}

void LLHUDNameTag::shiftAll(const LLVector3& offset)
{
    TextObjectIterator text_it;
    for (text_it = sTextObjects.begin(); text_it != sTextObjects.end(); ++text_it)
    {
        LLHUDNameTag *textp = text_it->get();
        textp->shift(offset);
    }
}

void LLHUDNameTag::shift(const LLVector3& offset)
{
    mPositionAgent += offset;
}

F32 LLHUDNameTag::getWorldHeight() const
{
    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    F32 height_meters = mLastDistance * (F32)tan(camera->getView() / 2.f);
    F32 height_pixels = camera->getViewHeightInPixels() / 2.f;
    F32 meters_per_pixel = height_meters / height_pixels;
    return mHeight * meters_per_pixel * gViewerWindow->getDisplayScale().mV[VY];
}

//static
void LLHUDNameTag::addPickable(std::set<LLViewerObject*> &pick_list)
{
    //this might put an object on the pick list a second time, overriding it's mGLName, which is ok
    // *FIX: we should probably cull against pick frustum
    VisibleTextObjectIterator text_it;
    for (text_it = sVisibleTextObjects.begin(); text_it != sVisibleTextObjects.end(); ++text_it)
    {
        pick_list.insert((*text_it)->mSourceObject);
    }
}

//static
// called when UI scale changes, to flush font width caches
void LLHUDNameTag::reshape()
{
    TextObjectIterator text_it;
    for (text_it = sTextObjects.begin(); text_it != sTextObjects.end(); ++text_it)
    {
        LLHUDNameTag* textp = (*text_it);
        std::vector<LLHUDTextSegment>::iterator segment_iter;
        for (segment_iter = textp->mTextSegments.begin();
             segment_iter != textp->mTextSegments.end(); ++segment_iter )
        {
            segment_iter->clearFontWidthMap();
        }
        for(segment_iter = textp->mLabelSegments.begin();
            segment_iter != textp->mLabelSegments.end(); ++segment_iter )
        {
            segment_iter->clearFontWidthMap();
        }
    }
}

//============================================================================

F32 LLHUDNameTag::LLHUDTextSegment::getWidth(const LLFontGL* font)
{
    std::map<const LLFontGL*, F32>::iterator iter = mFontWidthMap.find(font);
    if (iter != mFontWidthMap.end())
    {
        return iter->second;
    }
    else
    {
        F32 width = font->getWidthF32(mText.c_str());
        mFontWidthMap[font] = width;
        return width;
    }
}
