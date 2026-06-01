#include <pch.h>
#include <studio/collision/static_collision.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
constexpr int HDR_LENGTH = 80;
constexpr int HDR_VTX_OFFSET = 428;
constexpr int HDR_VVD_OFFSET = 432;
constexpr int HDR_VVC_OFFSET = 436;
constexpr int HDR_COLLISION_OFFSET = 460;
constexpr int HDR_STATIC_COLLISION_COUNT = 464;

[[noreturn]] void CollisionError(const char* fmt, ...)
{
	char message[2048];
	va_list args;
	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);
	throw std::runtime_error(message);
}

struct Vec3
{
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

struct Tri
{
	int a = 0;
	int b = 0;
	int c = 0;
};

struct Hull
{
	std::string name;
	std::vector<Vec3> verts;
	std::vector<Tri> faces;
};

struct ObjObject
{
	std::string name;
	std::vector<Tri> faces;
	std::vector<int> indices;
};

struct Plane
{
	Vec3 n;
	double d = 0.0;
};

struct EdgeBuild
{
	int a = 0;
	int b = 0;
	int face0 = 0;
	int face1 = 0;
};

struct Solid
{
	std::string name;
	int faceCount = 0;
	int planeCount = 0;
	int edgeCount = 0;
	int vertCount = 0;
	std::vector<std::uint8_t> data;
};

struct EdgeKey
{
	int a = 0;
	int b = 0;

	bool operator==(const EdgeKey& other) const
	{
		return a == other.a && b == other.b;
	}
};

struct EdgeKeyHash
{
	size_t operator()(const EdgeKey& key) const
	{
		return (static_cast<size_t>(key.a) << 32) ^ static_cast<size_t>(key.b);
	}
};

struct KdopSourceHull
{
	int sourceIndex = 0;
	std::string name;
	std::vector<Vec3> verts;
	double volume = 0.0;
};

struct KdopHull
{
	int sourceIndex = 0;
	std::string sourceName;
	std::vector<Vec3> verts;
	std::vector<std::vector<int>> faces;
};

Vec3 operator+(const Vec3& a, const Vec3& b)
{
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}

Vec3 operator-(const Vec3& a, const Vec3& b)
{
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

Vec3 operator/(const Vec3& a, double value)
{
	return { a.x / value, a.y / value, a.z / value };
}

Vec3 operator*(const Vec3& a, double value)
{
	return { a.x * value, a.y * value, a.z * value };
}

double Dot(const Vec3& a, const Vec3& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
	return {
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	};
}

double Length(const Vec3& a)
{
	return std::sqrt(Dot(a, a));
}

Vec3 Normalize(const Vec3& a)
{
	const double len = Length(a);
	return len > 0.000001 ? Vec3{ a.x / len, a.y / len, a.z / len } : Vec3{};
}

int ReadI32(const std::vector<std::uint8_t>& data, size_t offset)
{
	if (offset + 4 > data.size())
		CollisionError("read past end of model header at offset %zu", offset);

	int value = 0;
	std::memcpy(&value, data.data() + offset, sizeof(value));
	return value;
}

void WriteI32(std::vector<std::uint8_t>& data, size_t offset, int value)
{
	if (offset + 4 > data.size())
		CollisionError("write past end of model header at offset %zu", offset);

	std::memcpy(data.data() + offset, &value, sizeof(value));
}

void AppendI32(std::vector<std::uint8_t>& data, int value)
{
	const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(&value);
	data.insert(data.end(), p, p + sizeof(value));
}

void AppendF32(std::vector<std::uint8_t>& data, double value)
{
	const float f = static_cast<float>(value);
	const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(&f);
	data.insert(data.end(), p, p + sizeof(f));
}

std::vector<std::uint8_t> ReadFileBytes(const std::string& path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		CollisionError("couldn't open input file: %s", path.c_str());

	input.seekg(0, std::ios::end);
	const std::streamoff size = input.tellg();
	input.seekg(0, std::ios::beg);

	std::vector<std::uint8_t> data(static_cast<size_t>(size));
	if (!data.empty())
		input.read(reinterpret_cast<char*>(data.data()), data.size());
	return data;
}

void WriteFileBytes(const std::string& path, const std::vector<std::uint8_t>& data)
{
	const std::filesystem::path parentPath = std::filesystem::path(path).parent_path();
	if (!parentPath.empty())
		std::filesystem::create_directories(parentPath);

	std::ofstream output(path, std::ios::binary);
	if (!output)
		CollisionError("couldn't open output file: %s", path.c_str());

	output.write(reinterpret_cast<const char*>(data.data()), data.size());
}

int ParseObjFaceIndex(const std::string& token, int vertexCount)
{
	const size_t slash = token.find('/');
	const std::string indexText = slash == std::string::npos ? token : token.substr(0, slash);
	const int rawIndex = std::stoi(indexText);
	const int index = rawIndex < 0 ? vertexCount + rawIndex : rawIndex - 1;
	if (index < 0 || index >= vertexCount)
		CollisionError("OBJ face vertex index %i is out of range 1..%i", rawIndex, vertexCount);
	return index;
}

std::vector<Hull> ParseHullObj(const std::string& path)
{
	std::ifstream input(path);
	if (!input)
		CollisionError("couldn't open hull OBJ: %s", path.c_str());

	std::vector<Vec3> globalVerts;
	std::vector<ObjObject> objects;
	int currentObject = -1;

	auto ensureObject = [&]() -> ObjObject& {
		if (currentObject < 0)
		{
			ObjObject object;
			object.name = "hull_" + std::to_string(objects.size());
			objects.push_back(object);
			currentObject = static_cast<int>(objects.size()) - 1;
		}
		return objects[currentObject];
	};

	std::string line;
	while (std::getline(input, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();

		std::istringstream stream(line);
		std::string kind;
		stream >> kind;
		if (kind.empty() || kind[0] == '#')
			continue;

		if (kind == "o")
		{
			std::string name;
			std::getline(stream, name);
			if (!name.empty() && name[0] == ' ')
				name.erase(0, name.find_first_not_of(' '));

			ObjObject object;
			object.name = name.empty() ? "hull_" + std::to_string(objects.size()) : name;
			objects.push_back(object);
			currentObject = static_cast<int>(objects.size()) - 1;
			continue;
		}

		if (kind == "v")
		{
			Vec3 v;
			stream >> v.x >> v.y >> v.z;
			globalVerts.push_back(v);
			continue;
		}

		if (kind == "f")
		{
			std::vector<int> face;
			std::string token;
			while (stream >> token)
				face.push_back(ParseObjFaceIndex(token, static_cast<int>(globalVerts.size())));

			if (face.size() < 3)
				continue;

			ObjObject& object = ensureObject();
			for (size_t i = 1; i + 1 < face.size(); ++i)
				object.faces.push_back({ face[0], face[i], face[i + 1] });
			for (const int index : face)
				object.indices.push_back(index);
		}
	}

	std::vector<Hull> hulls;
	for (const ObjObject& object : objects)
	{
		if (object.faces.empty())
			continue;

		std::unordered_map<int, int> used;
		Hull hull;
		hull.name = object.name;

		for (const Tri& face : object.faces)
		{
			const int global[3] = { face.a, face.b, face.c };
			int local[3] = {};

			for (int i = 0; i < 3; ++i)
			{
				auto found = used.find(global[i]);
				if (found == used.end())
				{
					const int localIndex = static_cast<int>(hull.verts.size());
					used.emplace(global[i], localIndex);
					hull.verts.push_back(globalVerts[global[i]]);
					local[i] = localIndex;
				}
				else
				{
					local[i] = found->second;
				}
			}

			hull.faces.push_back({ local[0], local[1], local[2] });
		}

		if (!hull.faces.empty() && !hull.verts.empty())
			hulls.push_back(hull);
	}

	return hulls;
}

std::vector<KdopSourceHull> ParseKdopSourceObj(const std::string& path)
{
	std::ifstream input(path);
	if (!input)
		CollisionError("couldn't open source OBJ: %s", path.c_str());

	std::vector<Vec3> globalVerts;
	std::vector<ObjObject> objects;
	int currentObject = -1;

	auto ensureObject = [&]() -> ObjObject& {
		if (currentObject < 0)
		{
			ObjObject object;
			object.name = "hull_" + std::to_string(objects.size());
			objects.push_back(object);
			currentObject = static_cast<int>(objects.size()) - 1;
		}
		return objects[currentObject];
	};

	std::string line;
	while (std::getline(input, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();

		std::istringstream stream(line);
		std::string kind;
		stream >> kind;
		if (kind.empty() || kind[0] == '#')
			continue;

		if (kind == "o")
		{
			std::string name;
			std::getline(stream, name);
			if (!name.empty() && name[0] == ' ')
				name.erase(0, name.find_first_not_of(' '));

			ObjObject object;
			object.name = name.empty() ? "hull_" + std::to_string(objects.size()) : name;
			objects.push_back(object);
			currentObject = static_cast<int>(objects.size()) - 1;
			continue;
		}

		if (kind == "v")
		{
			Vec3 v;
			stream >> v.x >> v.y >> v.z;
			globalVerts.push_back(v);
			continue;
		}

		if (kind == "f")
		{
			ObjObject& object = ensureObject();
			std::string token;
			while (stream >> token)
				object.indices.push_back(ParseObjFaceIndex(token, static_cast<int>(globalVerts.size())));
		}
	}

	std::vector<KdopSourceHull> hulls;
	for (size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
	{
		ObjObject& object = objects[objectIndex];
		std::sort(object.indices.begin(), object.indices.end());
		object.indices.erase(std::unique(object.indices.begin(), object.indices.end()), object.indices.end());
		if (object.indices.size() < 4)
			continue;

		KdopSourceHull hull;
		hull.sourceIndex = static_cast<int>(objectIndex);
		hull.name = object.name;
		hull.verts.reserve(object.indices.size());
		for (const int index : object.indices)
			hull.verts.push_back(globalVerts[index]);

		Vec3 mn = hull.verts[0];
		Vec3 mx = hull.verts[0];
		for (const Vec3& v : hull.verts)
		{
			mn.x = min(mn.x, v.x);
			mn.y = min(mn.y, v.y);
			mn.z = min(mn.z, v.z);
			mx.x = max(mx.x, v.x);
			mx.y = max(mx.y, v.y);
			mx.z = max(mx.z, v.z);
		}
		hull.volume = (mx.x - mn.x) * (mx.y - mn.y) * (mx.z - mn.z);
		hulls.push_back(std::move(hull));
	}

	return hulls;
}

std::vector<Vec3> KdopDirections(int dop)
{
	if (dop != 18 && dop != 26 && dop != 50 && dop != 74)
		CollisionError("dop must be 18, 26, 50, or 74");

	std::vector<Vec3> dirs;
	std::vector<Vec3> seen;
	auto add = [&](Vec3 v) {
		v = Normalize(v);
		for (const Vec3& old : seen)
		{
			if (std::abs(old.x - v.x) < 1e-8 && std::abs(old.y - v.y) < 1e-8 && std::abs(old.z - v.z) < 1e-8)
				return;
		}
		seen.push_back(v);
		dirs.push_back(v);
	};

	for (int axis = 0; axis < 3; ++axis)
	{
		Vec3 v;
		if (axis == 0) v.x = 1.0;
		if (axis == 1) v.y = 1.0;
		if (axis == 2) v.z = 1.0;
		add(v);
		add(v * -1.0);
	}

	const int pairs[3][2] = { {0, 1}, {0, 2}, {1, 2} };
	for (const auto& pair : pairs)
	{
		for (int sa : { -1, 1 })
		{
			for (int sb : { -1, 1 })
			{
				Vec3 v;
				if (pair[0] == 0) v.x = sa;
				if (pair[0] == 1) v.y = sa;
				if (pair[0] == 2) v.z = sa;
				if (pair[1] == 0) v.x = sb;
				if (pair[1] == 1) v.y = sb;
				if (pair[1] == 2) v.z = sb;
				add(v);
			}
		}
	}

	if (dop == 26 || dop == 50 || dop == 74)
	{
		for (int sx : { -1, 1 })
			for (int sy : { -1, 1 })
				for (int sz : { -1, 1 })
					add({ static_cast<double>(sx), static_cast<double>(sy), static_cast<double>(sz) });
	}

	if (dop == 50 || dop == 74)
	{
		for (int zeroAxis = 0; zeroAxis < 3; ++zeroAxis)
		{
			std::vector<int> axes;
			for (int axis = 0; axis < 3; ++axis)
			{
				if (axis != zeroAxis)
					axes.push_back(axis);
			}

			for (int order = 0; order < 2; ++order)
			{
				const int majorAxis = axes[order == 0 ? 0 : 1];
				const int minorAxis = axes[order == 0 ? 1 : 0];
				for (int majorSign : { -1, 1 })
				{
					for (int minorSign : { -1, 1 })
					{
						Vec3 v;
						if (majorAxis == 0) v.x = majorSign * 2.0;
						if (majorAxis == 1) v.y = majorSign * 2.0;
						if (majorAxis == 2) v.z = majorSign * 2.0;
						if (minorAxis == 0) v.x = minorSign;
						if (minorAxis == 1) v.y = minorSign;
						if (minorAxis == 2) v.z = minorSign;
						add(v);
					}
				}
			}
		}
	}

	if (dop == 74)
	{
		for (int majorAxis = 0; majorAxis < 3; ++majorAxis)
		{
			for (int sx : { -1, 1 })
			{
				for (int sy : { -1, 1 })
				{
					for (int sz : { -1, 1 })
					{
						Vec3 v{ static_cast<double>(sx), static_cast<double>(sy), static_cast<double>(sz) };
						if (majorAxis == 0) v.x *= 2.0;
						if (majorAxis == 1) v.y *= 2.0;
						if (majorAxis == 2) v.z *= 2.0;
						add(v);
					}
				}
			}
		}
	}

	return dirs;
}

bool Solve3x3(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, Vec3& out)
{
	const double det = Dot(a, Cross(b, c));
	if (std::abs(det) < 1e-7)
		return false;
	out = (Cross(b, c) * d.x + Cross(c, a) * d.y + Cross(a, b) * d.z) / det;
	return true;
}

std::vector<Vec3> UniquePoints(const std::vector<Vec3>& points, double eps)
{
	std::vector<Vec3> out;
	for (const Vec3& p : points)
	{
		bool found = false;
		for (const Vec3& q : out)
		{
			if (Length(p - q) <= eps)
			{
				found = true;
				break;
			}
		}
		if (!found)
			out.push_back(p);
	}
	return out;
}

std::vector<int> SortedFaceIndices(const std::vector<Vec3>& points, const Vec3& normal, const std::vector<int>& faceIndices)
{
	Vec3 center;
	for (const int index : faceIndices)
		center = center + points[index];
	center = center / static_cast<double>(faceIndices.size());

	Vec3 seed{ 0.0, 0.0, 1.0 };
	if (std::abs(Dot(seed, normal)) > 0.9)
		seed = { 0.0, 1.0, 0.0 };
	Vec3 u = Normalize(Cross(seed, normal));
	Vec3 v = Cross(normal, u);

	std::vector<std::pair<double, int>> ordered;
	for (const int index : faceIndices)
	{
		const Vec3 rel = points[index] - center;
		ordered.push_back({ std::atan2(Dot(rel, v), Dot(rel, u)), index });
	}
	std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
		return a.first < b.first;
	});

	std::vector<int> result;
	result.reserve(ordered.size());
	for (const auto& item : ordered)
		result.push_back(item.second);
	return result;
}

bool BuildKdop(const std::vector<Vec3>& points, const std::vector<Vec3>& normals, double margin, double eps, KdopHull& out)
{
	std::vector<double> ds(normals.size(), -std::numeric_limits<double>::infinity());
	for (size_t i = 0; i < normals.size(); ++i)
	{
		for (const Vec3& p : points)
			ds[i] = max(ds[i], Dot(p, normals[i]));
		ds[i] += margin;
	}

	std::vector<Vec3> vertices;
	for (size_t i = 0; i < normals.size(); ++i)
	{
		for (size_t j = i + 1; j < normals.size(); ++j)
		{
			for (size_t k = j + 1; k < normals.size(); ++k)
			{
				Vec3 p;
				if (!Solve3x3(normals[i], normals[j], normals[k], { ds[i], ds[j], ds[k] }, p))
					continue;

				bool inside = true;
				for (size_t planeIndex = 0; planeIndex < normals.size(); ++planeIndex)
				{
					if (Dot(p, normals[planeIndex]) > ds[planeIndex] + eps)
					{
						inside = false;
						break;
					}
				}
				if (inside)
					vertices.push_back(p);
			}
		}
	}

	if (vertices.size() < 4)
		return false;

	vertices = UniquePoints(vertices, eps * 4.0);
	std::vector<std::vector<int>> faces;
	for (size_t planeIndex = 0; planeIndex < normals.size(); ++planeIndex)
	{
		std::vector<int> onPlane;
		for (size_t vertIndex = 0; vertIndex < vertices.size(); ++vertIndex)
		{
			if (std::abs(Dot(vertices[vertIndex], normals[planeIndex]) - ds[planeIndex]) <= eps * 8.0)
				onPlane.push_back(static_cast<int>(vertIndex));
		}
		if (onPlane.size() >= 3)
			faces.push_back(SortedFaceIndices(vertices, normals[planeIndex], onPlane));
	}

	if (faces.size() < 4)
		return false;

	out.verts = std::move(vertices);
	out.faces = std::move(faces);
	return true;
}

void WriteKdopObj(const std::string& path, const std::vector<KdopHull>& hulls)
{
	const std::filesystem::path parentPath = std::filesystem::path(path).parent_path();
	if (!parentPath.empty())
		std::filesystem::create_directories(parentPath);

	std::ofstream output(path);
	if (!output)
		CollisionError("couldn't open output OBJ: %s", path.c_str());

	int base = 1;
	for (size_t outIndex = 0; outIndex < hulls.size(); ++outIndex)
	{
		const KdopHull& hull = hulls[outIndex];
		output << "o kdop_" << std::setfill('0') << std::setw(3) << outIndex
			<< "_src_" << std::setw(3) << hull.sourceIndex << std::setfill(' ') << "\n";
		for (const Vec3& p : hull.verts)
			output << "v " << std::fixed << std::setprecision(8) << p.x << " " << p.y << " " << p.z << "\n";
		for (const std::vector<int>& face : hull.faces)
		{
			output << "f";
			for (const int index : face)
				output << " " << base + index;
			output << "\n";
		}
		base += static_cast<int>(hull.verts.size());
	}
}

bool BuildSolidFromHull(const Hull& hull, Solid& out)
{
	const std::vector<Vec3>& verts = hull.verts;
	std::vector<Tri> tris;
	for (const Tri& tri : hull.faces)
	{
		if (tri.a != tri.b && tri.b != tri.c && tri.a != tri.c)
			tris.push_back(tri);
	}

	if (verts.empty() || tris.empty())
		return false;

	Vec3 centroid;
	for (const Vec3& v : verts)
		centroid = centroid + v;
	centroid = centroid / static_cast<double>(verts.size());

	std::vector<Plane> facePlanes;
	std::vector<int> faceByTri;
	constexpr double EPS = 0.015;

	for (const Tri& tri : tris)
	{
		const Vec3& a = verts[tri.a];
		const Vec3& b = verts[tri.b];
		const Vec3& c = verts[tri.c];
		Vec3 n = Normalize(Cross(b - a, c - a));
		if (Length(n) < 0.000001)
			continue;

		double d = Dot(n, a);
		if (Dot(n, centroid) > d)
		{
			n = { -n.x, -n.y, -n.z };
			d = -d;
		}

		int face = -1;
		for (size_t i = 0; i < facePlanes.size(); ++i)
		{
			const Plane& plane = facePlanes[i];
			if (std::abs(Dot(plane.n, n) - 1.0) < EPS && std::abs(plane.d - d) < 0.25)
			{
				face = static_cast<int>(i);
				break;
			}
		}

		if (face < 0)
		{
			face = static_cast<int>(facePlanes.size());
			facePlanes.push_back({ n, d });
		}
		faceByTri.push_back(face);
	}

	std::unordered_map<EdgeKey, size_t, EdgeKeyHash> edgeLookup;
	std::vector<EdgeBuild> edges;

	for (size_t triIndex = 0; triIndex < tris.size(); ++triIndex)
	{
		const Tri& tri = tris[triIndex];
		const int indices[3] = { tri.a, tri.b, tri.c };
		const int face = triIndex < faceByTri.size() ? faceByTri[triIndex] : 0;

		for (int i = 0; i < 3; ++i)
		{
			const int a = indices[i];
			const int b = indices[(i + 1) % 3];
			const EdgeKey key = a < b ? EdgeKey{ a, b } : EdgeKey{ b, a };
			auto found = edgeLookup.find(key);
			if (found == edgeLookup.end())
			{
				EdgeBuild edge;
				edge.a = a;
				edge.b = b;
				edge.face0 = face;
				edge.face1 = face;
				edgeLookup.emplace(key, edges.size());
				edges.push_back(edge);
			}
			else
			{
				edges[found->second].face1 = face;
			}
		}
	}

	std::vector<EdgeBuild> edgeList;
	for (const EdgeBuild& edge : edges)
	{
		if (edge.face0 != edge.face1)
			edgeList.push_back(edge);
	}

	std::vector<Plane> bevelPlanes;
	for (const EdgeBuild& edge : edgeList)
	{
		if (edge.face0 < 0 || edge.face1 < 0 ||
			edge.face0 >= static_cast<int>(facePlanes.size()) ||
			edge.face1 >= static_cast<int>(facePlanes.size()))
		{
			continue;
		}

		const Plane& first = facePlanes[edge.face0];
		const Plane& second = facePlanes[edge.face1];
		const Vec3 n = Normalize(first.n + second.n);
		if (Length(n) < 0.000001)
			continue;

		bevelPlanes.push_back({ n, Dot(n, verts[edge.a]) });
	}

	out.name = hull.name;
	out.faceCount = static_cast<int>(facePlanes.size());
	out.planeCount = static_cast<int>(facePlanes.size() + bevelPlanes.size());
	out.edgeCount = static_cast<int>(edgeList.size());
	out.vertCount = static_cast<int>(verts.size());
	out.data.clear();
	out.data.reserve(out.planeCount * 16 + out.edgeCount * 28 + out.vertCount * 12);

	for (const Plane& plane : facePlanes)
	{
		AppendF32(out.data, plane.n.x);
		AppendF32(out.data, plane.n.y);
		AppendF32(out.data, plane.n.z);
		AppendF32(out.data, plane.d);
	}
	for (const Plane& plane : bevelPlanes)
	{
		AppendF32(out.data, plane.n.x);
		AppendF32(out.data, plane.n.y);
		AppendF32(out.data, plane.n.z);
		AppendF32(out.data, plane.d);
	}

	for (const EdgeBuild& edge : edgeList)
	{
		const Vec3& a = verts[edge.a];
		const Vec3& b = verts[edge.b];
		const Vec3 delta = b - a;

		AppendF32(out.data, a.x);
		AppendF32(out.data, a.y);
		AppendF32(out.data, a.z);
		AppendF32(out.data, delta.x);
		AppendF32(out.data, delta.y);
		AppendF32(out.data, delta.z);
		out.data.push_back(static_cast<std::uint8_t>(edge.face0 & 0xff));
		out.data.push_back(static_cast<std::uint8_t>(edge.face1 & 0xff));
		out.data.push_back(static_cast<std::uint8_t>(edge.a & 0xff));
		out.data.push_back(static_cast<std::uint8_t>(edge.b & 0xff));
	}

	for (const Vec3& vert : verts)
	{
		AppendF32(out.data, vert.x);
		AppendF32(out.data, vert.y);
		AppendF32(out.data, vert.z);
	}

	return true;
}

bool IsConvertibleSolid(const Solid& solid)
{
	return solid.faceCount > 3 &&
		solid.faceCount <= 255 &&
		solid.edgeCount > 3 &&
		solid.vertCount > 3 &&
		solid.vertCount <= 255;
}

std::vector<std::uint8_t> BuildCollisionBlock(const std::vector<Solid>& solids)
{
	std::vector<std::uint8_t> collision(solids.size() * 20, 0);
	size_t cursor = collision.size();

	for (size_t i = 0; i < solids.size(); ++i)
	{
		const Solid& solid = solids[i];
		const size_t headerOffset = i * 20;
		const int dataOffset = static_cast<int>(cursor - headerOffset);

		WriteI32(collision, headerOffset, solid.faceCount);
		WriteI32(collision, headerOffset + 4, solid.planeCount);
		WriteI32(collision, headerOffset + 8, solid.edgeCount);
		WriteI32(collision, headerOffset + 12, solid.vertCount);
		WriteI32(collision, headerOffset + 16, dataOffset);

		collision.insert(collision.end(), solid.data.begin(), solid.data.end());
		cursor += solid.data.size();
	}

	return collision;
}
}

StaticCollisionPatchResult PatchStaticCollisionFromObj(
	const std::string& modelPath,
	const std::string& hullObjPath,
	const std::string& outPath)
{
	std::vector<std::uint8_t> mdl = ReadFileBytes(modelPath);
	if (mdl.size() < HDR_STATIC_COLLISION_COUNT + 4)
		CollisionError("model file is too small: %s", modelPath.c_str());

	const int oldCollisionOffset = ReadI32(mdl, HDR_COLLISION_OFFSET);
	const int oldCollisionCount = ReadI32(mdl, HDR_STATIC_COLLISION_COUNT);
	const int vtxOffset = ReadI32(mdl, HDR_VTX_OFFSET);
	if (oldCollisionOffset < 0 || vtxOffset < oldCollisionOffset || vtxOffset > static_cast<int>(mdl.size()))
		CollisionError("model has invalid collision/vtx offsets: collision=%i vtx=%i size=%zu",
			oldCollisionOffset, vtxOffset, mdl.size());

	std::vector<Hull> hulls = ParseHullObj(hullObjPath);
	std::vector<Solid> solids;
	int dropped = 0;

	for (const Hull& hull : hulls)
	{
		Solid solid;
		if (!BuildSolidFromHull(hull, solid) || !IsConvertibleSolid(solid))
		{
			++dropped;
			continue;
		}
		solids.push_back(std::move(solid));
	}

	if (solids.empty())
		CollisionError("no convertible hulls found in %s", hullObjPath.c_str());

	std::vector<std::uint8_t> collision = BuildCollisionBlock(solids);
	std::vector<std::uint8_t> out;
	out.reserve(mdl.size() - (vtxOffset - oldCollisionOffset) + collision.size());
	out.insert(out.end(), mdl.begin(), mdl.begin() + oldCollisionOffset);
	out.insert(out.end(), collision.begin(), collision.end());
	out.insert(out.end(), mdl.begin() + vtxOffset, mdl.end());

	const int newCollisionOffset = oldCollisionOffset;
	const int newVtxOffset = newCollisionOffset + static_cast<int>(collision.size());
	const int delta = newVtxOffset - vtxOffset;

	WriteI32(out, HDR_LENGTH, static_cast<int>(out.size()));
	WriteI32(out, HDR_COLLISION_OFFSET, newCollisionOffset);
	WriteI32(out, HDR_STATIC_COLLISION_COUNT, static_cast<int>(solids.size()));
	if (ReadI32(out, HDR_VTX_OFFSET) > 0)
		WriteI32(out, HDR_VTX_OFFSET, ReadI32(out, HDR_VTX_OFFSET) + delta);
	if (ReadI32(out, HDR_VVD_OFFSET) > 0)
		WriteI32(out, HDR_VVD_OFFSET, ReadI32(out, HDR_VVD_OFFSET) + delta);
	if (ReadI32(out, HDR_VVC_OFFSET) > 0)
		WriteI32(out, HDR_VVC_OFFSET, ReadI32(out, HDR_VVC_OFFSET) + delta);

	WriteFileBytes(outPath, out);

	StaticCollisionPatchResult result;
	result.oldCollisionCount = oldCollisionCount;
	result.newCollisionCount = static_cast<int>(solids.size());
	result.collisionBytes = static_cast<int>(collision.size());
	result.droppedHullCount = dropped;
	return result;
}

std::vector<StaticCollisionBatchJob> ReadStaticCollisionBatchJobs(const std::string& path)
{
	std::ifstream input(path);
	if (!input)
		Error("couldn't open batch job file: %s\n", path.c_str());

	std::vector<StaticCollisionBatchJob> jobs;
	std::string line;
	int lineNumber = 0;
	while (std::getline(input, line))
	{
		++lineNumber;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;

		std::vector<std::string> cols;
		size_t start = 0;
		while (true)
		{
			const size_t tab = line.find('\t', start);
			cols.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
			if (tab == std::string::npos)
				break;
			start = tab + 1;
		}

		if (cols.size() != 3 || cols[0].empty() || cols[1].empty() || cols[2].empty())
			Error("invalid batch job at %s:%i; expected model<TAB>hulls.obj<TAB>out.mdl\n", path.c_str(), lineNumber);

		jobs.push_back({ cols[0], cols[1], cols[2] });
	}

	return jobs;
}

std::vector<StaticCollisionPipelineJob> ReadStaticCollisionPipelineJobs(const std::string& path)
{
	std::ifstream input(path);
	if (!input)
		Error("couldn't open pipeline job file: %s\n", path.c_str());

	std::vector<StaticCollisionPipelineJob> jobs;
	std::string line;
	int lineNumber = 0;
	while (std::getline(input, line))
	{
		++lineNumber;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;

		std::vector<std::string> cols;
		size_t start = 0;
		while (true)
		{
			const size_t tab = line.find('\t', start);
			cols.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
			if (tab == std::string::npos)
				break;
			start = tab + 1;
		}

		if (cols.size() != 3 || cols[0].empty() || cols[1].empty() || cols[2].empty())
			Error("invalid pipeline job at %s:%i; expected model<TAB>visible.obj<TAB>out.mdl\n", path.c_str(), lineNumber);

		jobs.push_back({ cols[0], cols[1], cols[2] });
	}

	return jobs;
}

void MakeKdopHullsFromObj(
	const std::string& inputObjPath,
	const std::string& outputObjPath,
	int dop,
	double margin,
	int maxHulls,
	double eps)
{
	std::vector<KdopSourceHull> sourceHulls = ParseKdopSourceObj(inputObjPath);
	if (sourceHulls.empty())
		CollisionError("no hulls found in %s", inputObjPath.c_str());

	std::sort(sourceHulls.begin(), sourceHulls.end(), [](const KdopSourceHull& a, const KdopSourceHull& b) {
		return a.volume > b.volume;
	});
	if (maxHulls > 0 && maxHulls < static_cast<int>(sourceHulls.size()))
		sourceHulls.resize(maxHulls);
	std::sort(sourceHulls.begin(), sourceHulls.end(), [](const KdopSourceHull& a, const KdopSourceHull& b) {
		return a.sourceIndex < b.sourceIndex;
	});

	const std::vector<Vec3> normals = KdopDirections(dop);
	std::vector<KdopHull> outHulls;
	for (const KdopSourceHull& source : sourceHulls)
	{
		KdopHull hull;
		hull.sourceIndex = source.sourceIndex;
		hull.sourceName = source.name;
		if (BuildKdop(source.verts, normals, margin, eps, hull))
			outHulls.push_back(std::move(hull));
	}

	if (outHulls.empty())
		CollisionError("no k-DOP hulls could be built from %s", inputObjPath.c_str());

	WriteKdopObj(outputObjPath, outHulls);
}

std::vector<StaticCollisionBatchJobResult> PatchStaticCollisionBatch(
	const std::vector<StaticCollisionBatchJob>& jobs,
	int threadCount)
{
	if (threadCount <= 0)
		threadCount = static_cast<int>(std::thread::hardware_concurrency());
	if (threadCount <= 0)
		threadCount = 1;
	threadCount = min(threadCount, static_cast<int>(jobs.size()));

	std::vector<StaticCollisionBatchJobResult> results(jobs.size());
	if (jobs.empty())
		return results;

	std::atomic<size_t> nextJob = 0;
	std::vector<std::thread> workers;
	workers.reserve(threadCount);

	for (int workerIndex = 0; workerIndex < threadCount; ++workerIndex)
	{
		workers.emplace_back([&]() {
			while (true)
			{
				const size_t index = nextJob.fetch_add(1);
				if (index >= jobs.size())
					return;

				StaticCollisionBatchJobResult result;
				result.job = jobs[index];
				try
				{
					result.patch = PatchStaticCollisionFromObj(
						result.job.modelPath,
						result.job.hullObjPath,
						result.job.outPath);
					result.ok = true;
				}
				catch (const std::exception& ex)
				{
					result.ok = false;
					result.error = ex.what();
				}
				catch (...)
				{
					result.ok = false;
					result.error = "unknown error";
				}
				results[index] = std::move(result);
			}
		});
	}

	for (std::thread& worker : workers)
		worker.join();

	return results;
}
