#include "collision.h"

#include <RE/H/hkpBroadPhase.h>
#include <RE/H/hkpBroadPhaseCastCollector.h>
#include <RE/H/hkpBvTreeShape.h>
#include <RE/H/hkpBoxShape.h>
#include <RE/H/hkpCapsuleShape.h>
#include <RE/H/hkpCollidable.h>
#include <RE/H/hkpConvexVerticesShape.h>
#include <RE/H/hkpShapeBuffer.h>
#include <RE/H/hkpShapeContainer.h>
#include <RE/H/hkpTypedBroadPhaseHandle.h>
#include <RE/H/hkpWorld.h>

namespace delirium
{
	namespace
	{
		constexpr float kUnit = 0.0142875f;  // metres per game unit
		using Type = RE::hkpShapeType;

		struct P
		{
			float x{}, y{}, z{};
		};
		P    operator+(P a, P b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		P    operator-(P a, P b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		P    operator*(P a, float s) { return { a.x * s, a.y * s, a.z * s }; }
		float Dot(P a, P b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		P    Cross(P a, P b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		P    Min(P a, P b) { return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) }; }
		P    Max(P a, P b) { return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) }; }
		P    Of(const RE::hkVector4& v) { return { v.quad.m128_f32[0], v.quad.m128_f32[1], v.quad.m128_f32[2] }; }
		RE::hkVector4 Hk(P p)
		{
			RE::hkVector4 v;
			v.quad = _mm_setr_ps(p.x, p.y, p.z, 0.0f);
			return v;
		}
		template <class T>
		const T& At(const void* a_base, std::size_t a_off)
		{
			return *reinterpret_cast<const T*>(static_cast<const char*>(a_base) + a_off);
		}

		// rigid transform, rotation by columns (Havok's layout): world = c0*x + c1*y + c2*z + t
		struct Xf
		{
			P c[3]{ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
			P t;
			P Apply(P v) const { return c[0] * v.x + c[1] * v.y + c[2] * v.z + t; }
			P Back(P v) const  // inverse
			{
				const P d = v - t;
				return { Dot(c[0], d), Dot(c[1], d), Dot(c[2], d) };
			}
		};
		Xf Of(const RE::hkTransform& a)
		{
			Xf x;
			x.c[0] = Of(a.rotation.col0);
			x.c[1] = Of(a.rotation.col1);
			x.c[2] = Of(a.rotation.col2);
			x.t = Of(a.translation);
			return x;
		}
		RE::hkTransform Hk(const Xf& a)
		{
			RE::hkTransform h;
			h.rotation.col0 = Hk(a.c[0]);
			h.rotation.col1 = Hk(a.c[1]);
			h.rotation.col2 = Hk(a.c[2]);
			h.translation = Hk(a.t);
			return h;
		}
		Xf Compose(const Xf& a, const Xf& b)  // a after b
		{
			Xf x;
			for (int i = 0; i < 3; ++i) {
				x.c[i] = a.c[0] * b.c[i].x + a.c[1] * b.c[i].y + a.c[2] * b.c[i].z;
			}
			x.t = a.Apply(b.t);
			return x;
		}
		bool Sane(const Xf& a)
		{
			for (int i = 0; i < 3; ++i) {
				const float l = Dot(a.c[i], a.c[i]);
				if (!(l > 0.9f && l < 1.1f)) {
					return false;
				}
			}
			return std::isfinite(a.t.x) && std::isfinite(a.t.y) && std::isfinite(a.t.z) && std::abs(a.t.x) < 1e6f && std::abs(a.t.y) < 1e6f &&
			       std::abs(a.t.z) < 1e6f;
		}
		bool ShapeOk(const RE::hkpShape* s)
		{
			return s && static_cast<std::uint32_t>(s->type) < static_cast<std::uint32_t>(Type::kTotal);
		}

		bool StaticLayer(RE::COL_LAYER a_l)
		{
			using L = RE::COL_LAYER;
			switch (a_l) {
			case L::kStatic:
			case L::kAnimStatic:
			case L::kTransparent:
			case L::kTrees:
			case L::kProps:
			case L::kTerrain:
			case L::kGround:
				return true;
			default:
				return false;
			}
		}

		struct Gather
		{
			P            qmin, qmax;  // the query box, Havok space
			P            origin;      // physics origin, Havok space
			float        toMetres{ 1 };
			std::size_t  maxTris{};
			Tris&        out;
			GatherStats& st;

			bool Full() const { return out.Count() >= maxTris; }
			bool Overlaps(P lo, P hi) const
			{
				return lo.x <= qmax.x && hi.x >= qmin.x && lo.y <= qmax.y && hi.y >= qmin.y && lo.z <= qmax.z && hi.z >= qmin.z;
			}
			void Tri(P a, P b, P c)  // Havok space, outward winding
			{
				if (!Overlaps(Min(Min(a, b), c), Max(Max(a, b), c))) {
					++st.culled;
					return;
				}
				if (Full()) {
					++st.capped;
					return;
				}
				const auto base = static_cast<std::uint32_t>(out.pos.size() / 3);
				for (const P& v : { a, b, c }) {
					const P m = (v - origin) * toMetres;
					out.pos.insert(out.pos.end(), { m.x, m.y, m.z });
				}
				out.idx.insert(out.idx.end(), { base, base + 1, base + 2 });
			}
			// a triangle whose outward side is away from `inside`
			void TriOut(P a, P b, P c, P inside)
			{
				if (Dot(Cross(b - a, c - a), (a + b + c) * (1.0f / 3) - inside) < 0) {
					std::swap(b, c);
				}
				Tri(a, b, c);
			}
			void Box(const Xf& x, P c, P h)  // local box
			{
				const P lo = c - h;
				P       v[8];
				for (int i = 0; i < 8; ++i) {
					v[i] = x.Apply({ i & 1 ? c.x + h.x : lo.x, i & 2 ? c.y + h.y : lo.y, i & 4 ? c.z + h.z : lo.z });
				}
				const P   mid = x.Apply(c);
				const int f[6][4]{ { 0, 2, 6, 4 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 5, 7, 6 } };
				for (const auto& q : f) {
					TriOut(v[q[0]], v[q[1]], v[q[2]], mid);
					TriOut(v[q[0]], v[q[2]], v[q[3]], mid);
				}
				++st.boxes;
			}
			// the shape's own bounds (Havok's virtual, always right) as a box in its frame
			void BoundsBox(const RE::hkpShape* s)
			{
				RE::hkAabb      a;
				const Xf        id;
				s->GetAabbImpl(Hk(id), 0.0f, a);
				const P lo = Of(a.min), hi = Of(a.max);
				cur_box = { (lo + hi) * 0.5f, (hi - lo) * 0.5f };
			}
			std::pair<P, P> cur_box;
			bool            AabbMatches(const RE::hkpShape* s, const Xf& x, P lo, P hi, float tol) const
			{
				RE::hkAabb a;
				s->GetAabbImpl(Hk(x), 0.0f, a);
				const P alo = Of(a.min), ahi = Of(a.max);
				const P dlo = lo - alo, dhi = ahi - hi;
				// ours must sit inside Havok's bounds and reach most of the way to them
				const float reach = tol * 4 + 0.1f;
				return dlo.x > -tol && dlo.y > -tol && dlo.z > -tol && dhi.x > -tol && dhi.y > -tol && dhi.z > -tol && dlo.x < reach &&
				       dlo.y < reach && dlo.z < reach && dhi.x < reach && dhi.y < reach && dhi.z < reach;
			}
			void Fallback(const RE::hkpShape* s, const Xf& x, const char* why)
			{
				BoundsBox(s);
				Box(x, cur_box.first, cur_box.second);
				++st.fallbacks;
				if (st.unknown.size() < 300) {
					st.unknown += fmt::format(" {}:{}", static_cast<int>(s->type), why);
				}
			}

			void Hull(const RE::hkpConvexVerticesShape* s, const Xf& x)
			{
				const int n = s->numVertices;
				const auto& rv = s->rotatedVertices;
				if (n < 4 || n > 4096 || rv.size() * 4 < n) {
					Fallback(s, x, "hull verts");
					return;
				}
				std::vector<P> v(static_cast<std::size_t>(n));
				P              lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
				for (int i = 0; i < n; ++i) {
					const auto& b = rv[i / 4];
					v[i] = { b.x.quad.m128_f32[i % 4], b.y.quad.m128_f32[i % 4], b.z.quad.m128_f32[i % 4] };
					lo = Min(lo, v[i]);
					hi = Max(hi, v[i]);
				}
				const P c = Of(s->aabbCenter), h = Of(s->aabbHalfExtents);
				const float tol = s->radius + 0.05f;
				const P dlo = lo - (c - h), dhi = (c + h) - hi;
				const bool fits = std::abs(dlo.x) < tol && std::abs(dlo.y) < tol && std::abs(dlo.z) < tol && std::abs(dhi.x) < tol &&
				                  std::abs(dhi.y) < tol && std::abs(dhi.z) < tol;
				const auto& planes = s->planeEquations;
				if (!fits || planes.size() < 4) {
					if (!fits) {
						Fallback(s, x, "hull bounds");
					} else {
						Box(x, c, h);  // no planes kept: its bounds
					}
					return;
				}
				const P   mid = x.Apply((lo + hi) * 0.5f);
				const float eps = 1e-3f * std::max({ hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 0.01f });
				for (int k = 0; k < planes.size(); ++k) {
					const P     nrm = Of(planes[k]);
					const float d = planes[k].quad.m128_f32[3];
					std::vector<P> face;
					for (const P& p : v) {
						if (std::abs(Dot(nrm, p) + d) < eps) {
							face.push_back(p);
						}
					}
					if (face.size() < 3) {
						continue;
					}
					P fc{};
					for (const P& p : face) {
						fc = fc + p;
					}
					fc = fc * (1.0f / static_cast<float>(face.size()));
					const P u0 = std::abs(nrm.x) < 0.9f ? P{ 1, 0, 0 } : P{ 0, 1, 0 };
					P       u = Cross(nrm, u0);
					u = u * (1.0f / std::sqrt(std::max(Dot(u, u), 1e-12f)));
					const P w = Cross(nrm, u);
					std::sort(face.begin(), face.end(), [&](const P& a, const P& b) {
						return std::atan2(Dot(a - fc, w), Dot(a - fc, u)) < std::atan2(Dot(b - fc, w), Dot(b - fc, u));
					});
					for (std::size_t i = 1; i + 1 < face.size(); ++i) {
						TriOut(x.Apply(face[0]), x.Apply(face[i]), x.Apply(face[i + 1]), mid);
					}
				}
				++st.hulls;
			}

			void Children(const RE::hkpShapeContainer* c, const RE::hkpShape* parent, const Xf& x, int depth, const RE::hkpShapeKey* keys,
				std::size_t nkeys)
			{
				RE::hkpShapeBuffer buf;
				auto               one = [&](RE::hkpShapeKey k) {
                    // a list's children carry their own layers (a mesh's triangles carry materials)
                    if (parent->type == Type::kList && !StaticLayer(c->GetCollisionFilterInfo(k).GetCollisionLayer()) &&
                        c->GetCollisionFilterInfo(k).GetCollisionLayer() != RE::COL_LAYER::kUnidentified) {
                        ++st.skippedLayer;
                        return;
                    }
                    const auto* child = c->GetChildShape(k, buf);
                    if (ShapeOk(child)) {
                        Walk(child, x, depth + 1);
                    }
				};
				if (keys) {
					for (std::size_t i = 0; i < nkeys && !Full(); ++i) {
						one(keys[i]);
					}
					return;
				}
				int guard = 0;
				for (auto k = c->GetFirstKey(); k != RE::HK_INVALID_SHAPE_KEY && guard < 200000 && !Full(); k = c->GetNextKey(k), ++guard) {
					one(k);
				}
			}

			void Walk(const RE::hkpShape* s, const Xf& x, int depth)
			{
				if (depth > 12 || Full()) {
					return;
				}
				++st.shapes;
				{
					RE::hkAabb a;  // the whole shape outside the query: nothing to do
					s->GetAabbImpl(Hk(x), 0.0f, a);
					if (!Overlaps(Of(a.min), Of(a.max))) {
						return;
					}
				}
				switch (s->type) {
				case Type::kTriangle:
					{
						const P a = At<P>(s, 0x30), b = At<P>(s, 0x40), c = At<P>(s, 0x50);
						const P wa = x.Apply(a), wb = x.Apply(b), wc = x.Apply(c);
						const float tol = static_cast<const RE::hkpConvexShape*>(s)->radius + 0.05f;
						if (!AabbMatches(s, x, Min(Min(wa, wb), wc), Max(Max(wa, wb), wc), tol)) {
							Fallback(s, x, "tri");
							return;
						}
						Tri(wa, wb, wc);
						++st.triangles;
						return;
					}
				case Type::kBox:
					{
						const auto* b = static_cast<const RE::hkpBoxShape*>(s);
						const float r = b->radius;
						Box(x, {}, Of(b->halfExtents) + P{ r, r, r });
						return;
					}
				case Type::kSphere:
				case Type::kCapsule:
				case Type::kCylinder:
					BoundsBox(s);
					Box(x, cur_box.first, cur_box.second);
					return;
				case Type::kConvexVertices:
					Hull(static_cast<const RE::hkpConvexVerticesShape*>(s), x);
					return;
				case Type::kConvexTranslate:
				case Type::kConvexTransform:
				case Type::kTransform:
					{
						const bool  conv = s->type != Type::kTransform;
						const auto* child = At<const RE::hkpShape*>(s, conv ? 0x30 : 0x28);
						Xf          local;
						if (s->type == Type::kConvexTranslate) {
							local.t = At<P>(s, 0x40);
						} else {
							local = Of(At<RE::hkTransform>(s, conv ? 0x40 : 0x50));
						}
						const Xf w = Compose(x, local);
						if (!ShapeOk(child) || !Sane(w)) {
							Fallback(s, x, "xf");
							return;
						}
						// the child at its transform must have the parent's bounds
						RE::hkAabb ca;
						child->GetAabbImpl(Hk(w), 0.0f, ca);
						if (!AabbMatches(s, x, Of(ca.min), Of(ca.max), 0.06f)) {
							Fallback(s, x, "xf bounds");
							return;
						}
						Walk(child, w, depth + 1);
						return;
					}
				case Type::kMOPP:
				case Type::kBVTree:
					{
						const auto* bv = static_cast<const RE::hkpBvTreeShape*>(s);
						const auto* c = bv->GetContainer();
						if (!c) {
							Fallback(s, x, "bv");
							return;
						}
						// the query box in the shape's frame
						P lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
						for (int i = 0; i < 8; ++i) {
							const P l = x.Back({ i & 1 ? qmax.x : qmin.x, i & 2 ? qmax.y : qmin.y, i & 4 ? qmax.z : qmin.z });
							lo = Min(lo, l);
							hi = Max(hi, l);
						}
						RE::hkAabb q;
						q.min = Hk(lo);
						q.max = Hk(hi);
						std::vector<RE::hkpShapeKey> keys(16384);
						const auto n = bv->QueryAabbImpl(q, keys.data(), static_cast<std::int32_t>(keys.size()));
						Children(c, s, x, depth, keys.data(), std::min<std::size_t>(n, keys.size()));
						return;
					}
				default:
					if (const auto* c = s->GetContainer()) {
						Children(c, s, x, depth, nullptr, 0);
						return;
					}
					Fallback(s, x, "type");
					return;
				}
			}
		};

		// a bad pointer in a shape we read by offset skips that body instead of crashing the game
		bool WalkGuarded(Gather* g, const RE::hkpShape* s, const Xf* x)
		{
			__try {
				g->Walk(s, *x, 0);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		class Collector : public RE::hkpBroadPhaseCastCollector
		{
		public:
			std::vector<const RE::hkpBroadPhaseHandle*> found;
			float AddBroadPhaseHandle(const RE::hkpBroadPhaseHandle* a_h, std::int32_t) override
			{
				if (a_h && found.size() < 4096) {
					found.push_back(a_h);
				}
				return 1.0f;
			}
		};
	}

	bool GatherStatic(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_center, const RE::NiPoint3& a_origin, float a_radius, float a_height,
		std::size_t a_maxTris, Tris& a_out, GatherStats& a_stats)
	{
		auto* bhk = a_cell ? a_cell->GetbhkWorld() : nullptr;
		auto* hk = bhk ? bhk->GetWorld1() : nullptr;
		if (!hk || !hk->broadPhase) {
			return false;
		}
		const float s = RE::bhkWorld::GetWorldScale();
		if (!(s > 0)) {
			return false;
		}
		const P c{ a_center.x * s, a_center.y * s, a_center.z * s };
		const float r = a_radius * s, h = a_height * s;
		Gather g{ c - P{ r, r, h }, c + P{ r, r, h }, P{ a_origin.x * s, a_origin.y * s, a_origin.z * s }, kUnit / s, a_maxTris, a_out, a_stats };

		RE::BSReadLockGuard lock(bhk->worldLock);
		Collector           col;
		RE::hkAabb          box;
		box.min = Hk(g.qmin);
		box.max = Hk(g.qmax);
		hk->broadPhase->QuerySingleAabbWithCollector(box, &col);
		for (const auto* h0 : col.found) {
			const auto* th = static_cast<const RE::hkpTypedBroadPhaseHandle*>(h0);
			if (th->ownerOffset == RE::hkpTypedBroadPhaseHandle::kInvalidOffset) {
				continue;
			}
			const auto* cd = reinterpret_cast<const RE::hkpCollidable*>(reinterpret_cast<const char*>(th) + th->ownerOffset);
			if (reinterpret_cast<const char*>(&cd->broadPhaseHandle) != reinterpret_cast<const char*>(th)) {
				continue;  // not a collidable's handle
			}
			if (!StaticLayer(cd->GetCollisionLayer())) {
				++a_stats.skippedLayer;
				continue;
			}
			if (!ShapeOk(cd->shape) || !cd->motion) {
				continue;
			}
			const Xf x = Of(*static_cast<const RE::hkTransform*>(cd->motion));
			if (!Sane(x)) {
				continue;
			}
			++a_stats.bodies;
			if (!WalkGuarded(&g, cd->shape, &x)) {
				// trace: whose body it was (her geometry is missing from the snapshot)
				const auto* ref = RE::TESHavokUtilities::FindCollidableRef(*cd);
				const auto* base = ref ? ref->GetBaseObject() : nullptr;
				a_stats.unknown += std::format(" crash[ref {:08X} base {:08X} '{}' layer {} shape type {}]", ref ? ref->GetFormID() : 0,
					base ? base->GetFormID() : 0, base ? base->GetName() : "", std::to_underlying(cd->GetCollisionLayer()),
					cd->shape ? std::to_underlying(cd->shape->type) : 0u);
			}
			if (g.Full()) {
				break;
			}
		}
		return true;
	}
}
