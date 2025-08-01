
#pragma once

#include "tarray.h"
#include "vectors.h"
#include "bounds.h"

#define LIGHTMAP_GLOBAL_SAMPLE_DISTANCE_MIN (int)4
#define LIGHTMAP_GLOBAL_SAMPLE_DISTANCE_MAX (int)64

class RectPackerItem;

struct LevelMeshSurface;

struct LightmapTileBinding
{
	uint32_t Type = 0;
	uint32_t TypeIndex = 0;
	uint32_t ControlSector = 0xffffffff;

	bool operator<(const LightmapTileBinding& other) const
	{
		if (TypeIndex != other.TypeIndex) return TypeIndex < other.TypeIndex;
		if (ControlSector != other.ControlSector) return ControlSector < other.ControlSector;
		return Type < other.Type;
	}
};

struct LightmapTile
{
	// Surface location in lightmap texture
	struct
	{
		int X = 0;
		int Y = 0;
		int Width = 0;
		int Height = 0;
		int ArrayIndex = 0;
		RectPackerItem* Item = nullptr;
		uint32_t Area() const { return Width * Height; }
	} AtlasLocation;

	// Calculate world coordinates to UV coordinates
	struct
	{
		FVector3 TranslateWorldToLocal = { 0.0f, 0.0f, 0.0f };
		FVector3 ProjLocalToU = { 0.0f, 0.0f, 0.0f };
		FVector3 ProjLocalToV = { 0.0f, 0.0f, 0.0f };
	} Transform;

	// Calculate world coordinates from UV coordinates
	struct
	{
		FVector3 WorldOrigin;
		FVector3 WorldU;
		FVector3 WorldV;
	} InverseTransform;

	int UseCount = 0;
	bool AddedThisFrame = false;

	LightmapTileBinding Binding;

	BBox Bounds;
	uint16_t SampleDimension = 0;
	FVector4 Plane = FVector4(0.0f, 0.0f, 1.0f, 0.0f);

	// Flats must always use XY axis (slopes may change rapidly in a way that causes artifacts otherwise)
	bool UseXYAxis = false;

	// Initial tile for surfaces that can be baked in the background
	bool NeedsInitialBake = false;

	// This tile MUST be baked before it can be rendered
	bool GeometryUpdate = true;

	// The light for this tile changed since last bake
	bool ReceivedNewLight = true;

	// Used to track if tile has already been added to the VisibleTiles list for this scene
	int LastSeen = 0;

	/*FVector2 ToUV(const FVector3& vert) const
	{
		FVector3 localPos = vert - Transform.TranslateWorldToLocal;
		float u = (localPos | Transform.ProjLocalToU) / AtlasLocation.Width;
		float v = (localPos | Transform.ProjLocalToV) / AtlasLocation.Height;
		return FVector2(u, v);
	}*/

	FVector2 ToUV(const FVector3& vert, float textureSize) const
	{
		FVector3 localPos = vert - Transform.TranslateWorldToLocal;
		float u = localPos | Transform.ProjLocalToU;
		float v = localPos | Transform.ProjLocalToV;

		// Clamp in case the wall moved outside the tile (happens if a lift moves with a static lightmap on it)
		u = std::max(std::min(u, (float)AtlasLocation.Width - 1.0f), 1.0f);
		v = std::max(std::min(v, (float)AtlasLocation.Height - 1.0f), 1.0f);

		u = (AtlasLocation.X + u) / textureSize;
		v = (AtlasLocation.Y + v) / textureSize;
		return FVector2(u, v);
	}

	enum PlaneAxis
	{
		AXIS_YZ = 0,
		AXIS_XZ,
		AXIS_XY
	};

	static PlaneAxis BestAxis(const FVector4& p)
	{
		float na = fabs(float(p.X));
		float nb = fabs(float(p.Y));
		float nc = fabs(float(p.Z));

		// figure out what axis the plane lies on
		if (na >= nb && na >= nc)
		{
			return AXIS_YZ;
		}
		else if (nb >= na && nb >= nc)
		{
			return AXIS_XZ;
		}

		return AXIS_XY;
	}

	void SetupTileTransform(int textureSize)
	{
		// These calculations align the tile so that there's a one texel border around the actual surface in the tile.
		// 
		// This removes sampling artifacts as a linear sampler reads from a 2x2 area.
		// The tile is also aligned to the grid to keep aliasing artifacts consistent.

		FVector3 uvMin;
		uvMin.X = std::floor(Bounds.min.X / SampleDimension) - 1.0f;
		uvMin.Y = std::floor(Bounds.min.Y / SampleDimension) - 1.0f;
		uvMin.Z = std::floor(Bounds.min.Z / SampleDimension) - 1.0f;

		FVector3 uvMax;
		uvMax.X = std::floor(Bounds.max.X / SampleDimension) + 2.0f;
		uvMax.Y = std::floor(Bounds.max.Y / SampleDimension) + 2.0f;
		uvMax.Z = std::floor(Bounds.max.Z / SampleDimension) + 2.0f;

		FVector3 tCoords[2] = { FVector3(0.0f, 0.0f, 0.0f), FVector3(0.0f, 0.0f, 0.0f) };
		int width, height;
		PlaneAxis planeAxis = UseXYAxis ? AXIS_XY : BestAxis(Plane);
		switch (planeAxis)
		{
		default:
		case AXIS_YZ:
			width = (int)std::round(uvMax.Y - uvMin.Y);
			height = (int)std::round(uvMax.Z - uvMin.Z);
			tCoords[0].Y = 1.0f / SampleDimension;
			tCoords[1].Z = 1.0f / SampleDimension;
			break;

		case AXIS_XZ:
			width = (int)std::round(uvMax.X - uvMin.X);
			height = (int)std::round(uvMax.Z - uvMin.Z);
			tCoords[0].X = 1.0f / SampleDimension;
			tCoords[1].Z = 1.0f / SampleDimension;
			break;

		case AXIS_XY:
			width = (int)std::round(uvMax.X - uvMin.X);
			height = (int)std::round(uvMax.Y - uvMin.Y);
			tCoords[0].X = 1.0f / SampleDimension;
			tCoords[1].Y = 1.0f / SampleDimension;
			break;
		}

		textureSize -= 6; // Lightmapper needs some padding when baking

		// Tile can never be bigger than the texture.
		if (width > textureSize)
		{
			tCoords[0] *= textureSize / (float)width;
			width = textureSize;
		}
		if (height > textureSize)
		{
			tCoords[1] *= textureSize / (float)height;
			height = textureSize;
		}

		uvMin *= (float)SampleDimension;
		uvMax *= (float)SampleDimension;

		Transform.TranslateWorldToLocal.X = uvMin.X + 0.1f;
		Transform.TranslateWorldToLocal.Y = uvMin.Y + 0.1f;
		Transform.TranslateWorldToLocal.Z = uvMin.Z + 0.1f;

		Transform.ProjLocalToU = tCoords[0];
		Transform.ProjLocalToV = tCoords[1];

		switch (planeAxis)
		{
		default:
		case AXIS_YZ:
			InverseTransform.WorldOrigin = PointAtYZ(uvMin.Y, uvMin.Z);
			InverseTransform.WorldU = PointAtYZ(uvMax.Y, uvMin.Z);
			InverseTransform.WorldV = PointAtYZ(uvMin.Y, uvMax.Z);
			break;

		case AXIS_XZ:
			InverseTransform.WorldOrigin = PointAtXZ(uvMin.X, uvMin.Z);
			InverseTransform.WorldU = PointAtXZ(uvMax.X, uvMin.Z);
			InverseTransform.WorldV = PointAtXZ(uvMin.X, uvMax.Z);
			break;

		case AXIS_XY:
			InverseTransform.WorldOrigin = PointAtXY(uvMin.X, uvMin.Y);
			InverseTransform.WorldU = PointAtXY(uvMax.X, uvMin.Y);
			InverseTransform.WorldV = PointAtXY(uvMin.X, uvMax.Y);
			break;
		}

		AtlasLocation.Width = width;
		AtlasLocation.Height = height;
	}

	FVector3 PointAtYZ(float y, float z) const { return FVector3(-(Plane.Y * y + Plane.Z * z + Plane.W) / Plane.X, y, z); }
	FVector3 PointAtXZ(float x, float z) const { return FVector3(x, -(Plane.X * x + Plane.Z * z + Plane.W) / Plane.Y, z); }
	FVector3 PointAtXY(float x, float y) const { return FVector3(x, y, -(Plane.X * x + Plane.Y * y + Plane.W) / Plane.Z); }
};
