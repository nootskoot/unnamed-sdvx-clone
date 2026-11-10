#include "stdafx.h"
#include "LaserTrackBuilder.hpp"
#include <Beatmap/BeatmapPlayback.hpp>
#include "Track.hpp"
#include "GameConfig.hpp"
#include <algorithm>
#include <vector>
#include <cmath>

using Shared::Rect;
using Shared::Rect3D;


// Preserve the *original* USC laser triangles (including slam corners) when
// clipping to a local Y window. Never fabricate a substitute ribbon.
// Position and UV are interpolated together to avoid texture discontinuities.
namespace
{
using LaserVertex = MeshGenerators::SimpleVertex;

LaserVertex IntersectLaserY(const LaserVertex& a, const LaserVertex& b, float boundary)
{
    const double denominator = double(b.pos.y) - double(a.pos.y);
    const double t = denominator == 0.0 ? 0.0 :
        std::max(0.0, std::min(1.0, (double(boundary) - a.pos.y) / denominator));
    const float f = float(t);
    return LaserVertex(
        Vector3(a.pos.x + (b.pos.x - a.pos.x) * f,
                boundary,
                a.pos.z + (b.pos.z - a.pos.z) * f),
        Vector2(a.tex.x + (b.tex.x - a.tex.x) * f,
                a.tex.y + (b.tex.y - a.tex.y) * f));
}

std::vector<LaserVertex> ClipLaserPolygonY(const std::vector<LaserVertex>& input,
                                           float boundary, bool keepAbove)
{
    std::vector<LaserVertex> output;
    if (input.empty())
        return output;
    auto inside = [&](const LaserVertex& v) {
        return keepAbove ? v.pos.y >= boundary : v.pos.y <= boundary;
    };
    LaserVertex previous = input.back();
    bool previousInside = inside(previous);
    for (const LaserVertex& current : input)
    {
        const bool currentInside = inside(current);
        if (previousInside != currentInside)
            output.push_back(IntersectLaserY(previous, current, boundary));
        if (currentInside)
            output.push_back(current);
        previous = current;
        previousInside = currentInside;
    }
    return output;
}

void ClipOriginalLaserTrianglesToY(Vector<LaserVertex>& vertices,
                                   float minY, float maxY)
{
    Vector<LaserVertex> clipped;
    if (!(minY < maxY) || !std::isfinite(minY) || !std::isfinite(maxY))
    {
        vertices.clear();
        return;
    }
    // Both regular USC lasers and slam/corner meshes use TriangleList.
    for (size_t i = 0; i + 2 < vertices.size(); i += 3)
    {
        const auto& a = vertices[i];
        const auto& b = vertices[i + 1];
        const auto& c = vertices[i + 2];
        if (!std::isfinite(a.pos.x) || !std::isfinite(a.pos.y) ||
            !std::isfinite(b.pos.x) || !std::isfinite(b.pos.y) ||
            !std::isfinite(c.pos.x) || !std::isfinite(c.pos.y))
            continue;
        std::vector<LaserVertex> polygon = { a, b, c };
        polygon = ClipLaserPolygonY(polygon, minY, true);
        polygon = ClipLaserPolygonY(polygon, maxY, false);
        // Fan triangulation preserves winding and all original UV seams.
        for (size_t p = 1; p + 1 < polygon.size(); ++p)
        {
            clipped.Add(polygon[0]);
            clipped.Add(polygon[p]);
            clipped.Add(polygon[p + 1]);
        }
    }
    vertices.clear();
    for (const LaserVertex& vertex : clipped)
        vertices.Add(vertex);
}
} // namespace

LaserTrackBuilder::LaserTrackBuilder(class OpenGL* gl, class Track* track, uint32 laserIndex)
{
	m_gl = gl;
	m_track = track;
	m_laserIndex = laserIndex;
	m_trackWidth = track->trackWidth;
	m_laserWidth = track->laserWidth;
	laserTextureSize = track->laserTextures[0]->GetSize(); // NOTE: expects left/right textures to be the same size!
	laserEntryTextureSize = track->laserTailTextures[0]->GetSize();
	laserExitTextureSize = track->laserTailTextures[2]->GetSize();
}
float LaserTrackBuilder::SlamLength(BeatmapPlayback& playback, LaserObjectState* laser)
{
    if (playback.cMod || !playback.GetBeatmap().GetKSONScroll())
        return playback.ToViewDistance(laser->time, slamDuration) * laserLengthScale;
    const auto& scroll = *playback.GetBeatmap().GetKSONScroll();
    const double t = playback.GetBeatmap().GetVisualTimes(*laser).first;
    const double travel = (scroll.Position(t + slamDuration) - scroll.Position(t)) * laserLengthScale;
    // A slam is a textured cap, not a hold spanning a teleport. Keep one
    // shared cap length for its body, continuation and exit texture.
    const double cap = std::max(0.001, double(actualLaserHeight));
    return float(std::clamp(travel, -cap, cap));
}

Mesh LaserTrackBuilder::GenerateTrackMesh(class BeatmapPlayback& playback, LaserObjectState* laser)
{
    return GenerateTrackMeshInternal(playback, laser, false, 0.0f, 0.0f);
}

Mesh LaserTrackBuilder::GenerateTrackMeshInternal(class BeatmapPlayback& playback,
    LaserObjectState* laser, bool clip, float clipMinY, float clipMaxY)
{
	if(!clip && m_objectCache.Contains(laser))
		return m_objectCache[laser];

	Mesh newMesh = MeshRes::Create(m_gl);

	m_nativeCache = !playback.cMod && playback.GetBeatmap().GetKSONScroll();
    const float length = playback.ToViewDistance(laser->time, laser->duration);

	if((laser->flags & LaserObjectState::flag_Instant) != 0) // Slam segment
	{
		float left, right, offsetT, offsetB, uvT, uvB;
		left = laser->points[0] * effectiveWidth - effectiveWidth * 0.5f;
		right = laser->points[1] * effectiveWidth - effectiveWidth * 0.5f;
		float halfWidth = actualLaserWidth * 0.5f;
		offsetT = -actualLaserWidth;
		offsetB = -offsetT;
		uvB = 1.5f;
		uvT = -0.5f;


		if ((laser->flags & LaserObjectState::flag_Extended) != 0)
		{
			left = (laser->points[0] * 2.0f - 0.5f) * effectiveWidth - effectiveWidth * 0.5f;
			right = (laser->points[1] * 2.0f - 0.5f) * effectiveWidth - effectiveWidth * 0.5f;
		}

		// If corners should be placed, connecting the texture to the previous laser
		bool swapped = false;


		if(laser->points[0] > laser->points[1])
		{
			// <------
			std::swap(left, right);
			std::swap(offsetT, offsetB);
			std::swap(uvB, uvT);
			swapped = true;
		}// else ------>

		// Generate positions for middle top and bottom
		float slamLength = SlamLength(playback, laser);
		float halfLength = slamLength * 0.5;
		Rect3D centerMiddle = Rect3D(left, slamLength + halfLength, right, -halfLength);

		Vector<MeshGenerators::SimpleVertex> verts =
		{
			{ { centerMiddle.Left() + offsetB, centerMiddle.Bottom(),  0.0f },{ uvB, 0.0f } }, // BL
			{ { centerMiddle.Right() + offsetB, centerMiddle.Bottom(),  0.0f },{ uvB, 1.0f } }, // BR
			{ { centerMiddle.Right() + offsetT, centerMiddle.Top(),  0.0f },{ uvT, 1.0f } }, // TR

			{ { centerMiddle.Left() + offsetB, centerMiddle.Bottom(),  0.0f },{ uvB, 0.0f } }, // BL
			{ { centerMiddle.Right() + offsetT, centerMiddle.Top(),  0.0f },{ uvT, 1.0f } }, // TR
			{ { centerMiddle.Left() + offsetT, centerMiddle.Top(),  0.0f },{ uvT, 0.0f } }, // TL
		};

		// Generate left corner
		{
			Rect3D leftCenter = Rect3D(left - actualLaserWidth, centerMiddle.Top() - halfLength, left + halfWidth, centerMiddle.Bottom() + halfLength);

			Vector<MeshGenerators::SimpleVertex> leftVerts;
			if(swapped)
			{

				leftVerts =
				{
					{ { leftCenter.Right(), leftCenter.Top(),  0.0f },{ 1.0f, 0.0f } }, // BR
					{ { leftCenter.Left(), leftCenter.Top(),  0.0f },{ -0.5f, 0.0f } }, // BL
					{ { leftCenter.Left(), leftCenter.Bottom() - halfLength,  0.0f },{ -0.5f, 1.0f } }, // TL
				};

			}
			else
			{
				leftVerts =
				{
					{ { leftCenter.Left(), leftCenter.Bottom(),  0.0f },{ -0.5f, 0.0f } }, // BL
					{ { leftCenter.Right(), leftCenter.Bottom(),  0.0f },{ 1.0f, 0.0f } }, // BR
					{ { leftCenter.Left(), leftCenter.Top() + halfLength,  0.0f },{ -0.5f, 1.0f } }, // TL
				};
			}
			for (auto& v : leftVerts)
				verts.Add(v);
		}

		// Generate right corner
		{
			Rect3D rightCenter = Rect3D(right - halfWidth, centerMiddle.Top() - halfLength, right + actualLaserWidth, centerMiddle.Bottom() + halfLength);
			Vector<MeshGenerators::SimpleVertex> rightVerts;
			if(swapped)
			{
				rightVerts =
				{
					{ { rightCenter.Left(), rightCenter.Bottom(),  0.0f },{ 0.0f, 0.0f } }, // BL
					{ { rightCenter.Right(), rightCenter.Bottom(),  0.0f },{ 1.5f, 1.0f } }, // BR
					{ { rightCenter.Right(), rightCenter.Top() + halfLength,  0.0f },{ 1.5f, 1.0f } }, // TR
				};
			}
			else
			{
				rightVerts =
				{
					{ { rightCenter.Right(), rightCenter.Bottom() - halfLength,  0.0f },{ 1.5f, 1.0f } }, // TR
					{ { rightCenter.Right(), rightCenter.Top(),  0.0f },{ 1.5f, 1.0f } }, // BR
					{ { rightCenter.Left(), rightCenter.Top(),  0.0f },{ 0.0f, 0.0f } }, // BL
				};
			}

			for (auto& v : rightVerts)
				verts.Add(v);
		}

        if (m_nativeCache && SlamLength(playback, laser) < 0)
            for (size_t i=0; i+2<verts.size(); i+=3) std::swap(verts[i+1], verts[i+2]);
		if (clip)
            ClipOriginalLaserTrianglesToY(verts, clipMinY, clipMaxY);
        if (verts.empty())
            return Mesh();
        newMesh->SetData(verts);
		newMesh->SetPrimitiveType(PrimitiveType::TriangleList);
	}
    else if (m_nativeCache)
    {
        const auto& beatmap = playback.GetBeatmap();
        const auto& scroll = *beatmap.GetKSONScroll();
        const auto times = beatmap.GetVisualTimes(*laser);
        const auto& path = beatmap.GetVisualPath(*laser);
        const double origin = scroll.Position(times.first);
        const double p0 = scroll.Pulse(times.first), p1 = scroll.Pulse(times.second);
        const float startShift = laser->prev && (laser->prev->flags & LaserObjectState::flag_Instant)
            ? SlamLength(playback, laser->prev) : 0.0f;
        Vector<MeshGenerators::SimpleVertex> verts;
        auto point = [&](size_t i) {
            const double fraction = p1 > p0 ? (scroll.Pulse(path[i].first)-p0)/(p1-p0) : 0;
            double x = laser->points[0] + (laser->points[1]-laser->points[0])*fraction;
            if (laser->flags & LaserObjectState::flag_Extended) x = 2*x-0.5;
            return Vector2(float(x*effectiveWidth-effectiveWidth*0.5),
                float((path[i].second-origin)*laserLengthScale) + (i==0 ? startShift : 0));
        };
        for (size_t i=1; i<path.size(); ++i) {
            Vector2 a=point(i-1), b=point(i);
            // Keep front-facing triangles for both scroll directions.
            if (a.y > b.y) std::swap(a,b);
            if (std::abs(b.y-a.y)<1e-7f) continue;
            const float v=(b.y-a.y)/actualLaserHeight;
            verts.Add({{a.x-actualLaserWidth,a.y,0},{-0.5f,v}});
            verts.Add({{a.x+actualLaserWidth,a.y,0},{1.5f,v}});
            verts.Add({{b.x+actualLaserWidth,b.y,0},{1.5f,0}});
            verts.Add({{a.x-actualLaserWidth,a.y,0},{-0.5f,v}});
            verts.Add({{b.x+actualLaserWidth,b.y,0},{1.5f,0}});
            verts.Add({{b.x-actualLaserWidth,b.y,0},{-0.5f,0}});
        }
        newMesh->SetData(verts);
        newMesh->SetPrimitiveType(PrimitiveType::TriangleList);
    }
	else
	{
		float prevLength = 0.0f;
		if(laser->prev && (laser->prev->flags & LaserObjectState::flag_Instant) != 0)
		{
			// Previous slam length
			prevLength = SlamLength(playback, laser->prev);
		}

		Vector2 points[2];

		// Connecting center points
		points[0] = Vector2(laser->points[0] * effectiveWidth - effectiveWidth * 0.5f, prevLength); // Bottom
		points[1] = Vector2(laser->points[1] * effectiveWidth - effectiveWidth * 0.5f, length * laserLengthScale); // Top
		if ((laser->flags & LaserObjectState::flag_Extended) != 0)
		{
			points[0] = Vector2((laser->points[0] * 2.0f - 0.5f) * effectiveWidth - effectiveWidth * 0.5f, prevLength); // Bottom
			points[1] = Vector2((laser->points[1] * 2.0f - 0.5f) * effectiveWidth - effectiveWidth * 0.5f, length * laserLengthScale); // Top
		}

		float uMin = -0.5f;
		float uMax = 1.5f;

		float vMin = 0.0f;
		float vMax = (int)((length * laserLengthScale) / actualLaserHeight);

		Vector<MeshGenerators::SimpleVertex> verts =
		{
			{ { points[0].x - actualLaserWidth, points[0].y,  0.0f },{ uMin, vMax } }, // BL
			{ { points[0].x + actualLaserWidth, points[0].y,  0.0f },{ uMax, vMax } }, // BR
			{ { points[1].x + actualLaserWidth, points[1].y,  0.0f },{ uMax, vMin } }, // TR

			{ { points[0].x - actualLaserWidth, points[0].y,  0.0f },{ uMin, vMax } }, // BL
			{ { points[1].x + actualLaserWidth, points[1].y,  0.0f },{ uMax, vMin } }, // TR
			{ { points[1].x - actualLaserWidth, points[1].y,  0.0f },{ uMin, vMin } }, // TL
		};


		if (clip)
            ClipOriginalLaserTrianglesToY(verts, clipMinY, clipMaxY);
        if (verts.empty())
            return Mesh();
        newMesh->SetData(verts);
		newMesh->SetPrimitiveType(PrimitiveType::TriangleList);
	}

	// Cache this mesh
	if (!clip)
		m_objectCache.Add(laser, newMesh);
	return newMesh;
}

// Build camera-local geometry for parts of very long/reversed lasers.
// Never cache a clipped mesh: the window changes every frame.
// Draw-time clipping only: the clipped mesh is never cached, because the
// local camera window changes each frame. Fully visible lasers use the
// original cached mesh without generating new GPU objects.
Mesh LaserTrackBuilder::GenerateClippedTrackMesh(class BeatmapPlayback& playback,
    LaserObjectState* laser, float clipMinY, float clipMaxY)
{
    if (!std::isfinite(clipMinY) || !std::isfinite(clipMaxY) || !(clipMinY < clipMaxY))
        return Mesh();

    float lowY, highY;
    if ((laser->flags & LaserObjectState::flag_Instant) != 0)
    {
        // Conservative bounds include the triangular slam wings.
        const float travel = SlamLength(playback, laser);
        const float reach = 2.0f * std::abs(travel);
        lowY = -reach;
        highY = reach;
    }
    else
    {
        float fromY = 0.0f;
        if (laser->prev && (laser->prev->flags & LaserObjectState::flag_Instant) != 0)
            fromY = SlamLength(playback, laser->prev);
        const float toY = playback.ToViewDistance(laser->time, laser->duration) * laserLengthScale;
        lowY = std::min(fromY, toY);
        highY = std::max(fromY, toY);
    }
    if (!std::isfinite(lowY) || !std::isfinite(highY))
        return Mesh();
    if (highY < clipMinY || lowY > clipMaxY)
        return Mesh();
    if (lowY >= clipMinY && highY <= clipMaxY)
        return GenerateTrackMesh(playback, laser);
    return GenerateTrackMeshInternal(playback, laser, true, clipMinY, clipMaxY);
}

Mesh LaserTrackBuilder::GenerateTrackEntry(class BeatmapPlayback& playback, LaserObjectState* laser)
{
	assert(laser->prev == nullptr);
	if(m_cachedEntries.Contains(laser))
		return m_cachedEntries[laser];

	Mesh newMesh = MeshRes::Create(m_gl);

	// Starting point of laser
	float startingX = laser->points[0] * effectiveWidth - effectiveWidth * 0.5f;
	if ((laser->flags & LaserObjectState::flag_Extended) != 0)
		startingX = (laser->points[0] * 2.0f - 0.5f) * effectiveWidth - effectiveWidth * 0.5f;



	// Length of the tail
	float length = (float)laserEntryTextureSize.y / (float)laserEntryTextureSize.x * actualLaserWidth;

	Vector<MeshGenerators::SimpleVertex> verts;
	Rect3D pos = Rect3D(Vector2(startingX - actualLaserWidth, -length), Vector2(actualLaserWidth * 2, length));
	Rect uv = Rect(-0.5f, 0.0f, 1.5f, 1.0f);
	MeshGenerators::GenerateSimpleXYQuad(pos, uv, verts);

	newMesh->SetData(verts);
	newMesh->SetPrimitiveType(PrimitiveType::TriangleList);

	// Cache this mesh
	m_cachedEntries.Add(laser, newMesh);
	return newMesh;

}
Mesh LaserTrackBuilder::GenerateTrackExit(class BeatmapPlayback& playback, LaserObjectState* laser)
{
	assert(laser->next == nullptr);
	if(m_cachedExits.Contains(laser))
		return m_cachedExits[laser];

	Mesh newMesh = MeshRes::Create(m_gl);

	// Ending point of laser 
	float startingX = laser->points[1] * effectiveWidth - effectiveWidth * 0.5f;
	if ((laser->flags & LaserObjectState::flag_Extended) != 0)
		startingX = (laser->points[1] * 2.0f - 0.5f) * effectiveWidth - effectiveWidth * 0.5f;

	// Length of the tail
	float length = (float)laserExitTextureSize.y / (float)laserExitTextureSize.x * actualLaserWidth;

	// Length of this segment
	float prevLength = 0.0f;
	if((laser->flags & LaserObjectState::flag_Instant) != 0)
	{
		prevLength = SlamLength(playback, laser);
	}
	else
	{
		prevLength = playback.ToViewDistance(laser->time, laser->duration) * laserLengthScale;
	}

	Vector<MeshGenerators::SimpleVertex> verts;
	Rect3D pos = Rect3D(Vector2(startingX - actualLaserWidth, prevLength), Vector2(actualLaserWidth * 2, length));
	Rect uv = Rect(-0.5f, 0.0f, 1.5f, 1.0f);
	MeshGenerators::GenerateSimpleXYQuad(pos, uv, verts);

	newMesh->SetData(verts);
	newMesh->SetPrimitiveType(PrimitiveType::TriangleList);

	// Cache this mesh
	m_cachedExits.Add(laser, newMesh);
	return newMesh;
}

float LaserTrackBuilder::GetLaserLengthScaleAt(MapTime time)
{
	/// TODO: return scale based on speed of timing point to change horizontal laser thickness
	return 1.0f;
}

void LaserTrackBuilder::m_RecalculateConstants()
{
	// Calculate amount to scale laser size to fit the texture border in
	//assert(laserTextureSize.x == laserTextureSize.y); // Use Square texture
	const float laserCenterAmount = ((float)laserTextureSize.x - ((float)laserBorderPixels * 2)) / (float)(laserTextureSize.x);
	const float laserBorderAmount = (1.0f - laserCenterAmount);

	// The uv coordinates to sample the laser without the border, or only the border
	textureBorder = (float)laserBorderPixels / laserTextureSize.x;
	invTextureBorder = 1.0f - textureBorder;

	// The the size of the laser with compensation added for the border
	actualLaserWidth = m_laserWidth;
	actualLaserHeight = actualLaserWidth * laserTextureSize.y / laserTextureSize.x;

	// The width of the laser without the border
	laserWidthNoBorder = actualLaserWidth * laserCenterAmount;
	// World size of a single border around a laser
	realBorderSize = (actualLaserWidth - laserWidthNoBorder) * 0.5f;

	// The length of the horizontal slam segments
	slamDuration = 50 * g_gameConfig.GetFloat(GameConfigKeys::SlamThicknessMultiplier);

	// The effective area in which the center point of the laser can move
	effectiveWidth = m_trackWidth - m_laserWidth;
}

void LaserTrackBuilder::m_Cleanup(MapTime newTime, Map<LaserObjectState*, Mesh>& arr)
{
	// Cleanup unused meshes
	for(auto it = arr.begin(); it != arr.end();)
	{
		LaserObjectState* obj = it->first;
		MapTime endTime = obj->time + obj->duration + 1000;
		if(newTime > endTime)
		{
			it = arr.erase(it);
			continue;
		}
		it++;
	}
}
void LaserTrackBuilder::Reset()
{
    m_nativeCache = false;
	m_objectCache.clear();
	m_cachedEntries.clear();
	m_cachedExits.clear();
	m_RecalculateConstants();
}
void LaserTrackBuilder::Update(MapTime newTime)
{
    // Past native notes can return into view. Retain their cached meshes until Reset.
    if (m_nativeCache) return;
	m_Cleanup(newTime, m_objectCache);
	m_Cleanup(newTime, m_cachedEntries);
	m_Cleanup(newTime, m_cachedExits);
}