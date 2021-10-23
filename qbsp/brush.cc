/*
    Copyright (C) 1996-1997  Id Software, Inc.
    Copyright (C) 1997       Greg Lewis
    Copyright (C) 1999-2005  Id Software, Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

#include <cstring>

#include <qbsp/qbsp.hh>

/*
 * Beveled clipping hull can generate many extra faces
 */
constexpr size_t MAX_FACES = 128;
constexpr size_t MAX_HULL_POINTS = 512;
constexpr size_t MAX_HULL_EDGES = 1024;

struct hullbrush_t
{
    const mapbrush_t *srcbrush;
    contentflags_t contents;
    aabb3d bounds;

    std::vector<mapface_t> faces;
    std::vector<qvec3d> points;
    std::vector<qvec3d> corners;
    std::vector<std::tuple<int, int>> edges;

    int linenum;
};

/*
=================
Face_Plane
=================
*/
qplane3d Face_Plane(const face_t *face)
{
    qplane3d result = map.planes.at(face->planenum);

    if (face->planeside) {
        return -result;
    }

    return result;
}

/*
=================
CheckFace

Note: this will not catch 0 area polygons
=================
*/
static void CheckFace(face_t *face, const mapface_t &sourceface)
{
    const qbsp_plane_t &plane = map.planes[face->planenum];

    if (face->w.size() < 3) {
        if (face->w.size() == 2) {
            FError("line {}: too few points (2): ({}) ({})\n", sourceface.linenum, face->w[0], face->w[1]);
        } else if (face->w.size() == 1) {
            FError("line {}: too few points (1): ({})\n", sourceface.linenum, face->w[0]);
        } else {
            FError("line {}: too few points ({})", sourceface.linenum, face->w.size());
        }
    }

    qvec3d facenormal = plane.normal;
    if (face->planeside)
        facenormal = -facenormal;

    for (size_t i = 0; i < face->w.size(); i++) {
        const qvec3d &p1 = face->w[i];
        const qvec3d &p2 = face->w[(i + 1) % face->w.size()];

        for (auto &v : p1)
            if (v > options.worldExtent || v < -options.worldExtent)
                FError("line {}: coordinate out of range ({})", sourceface.linenum, v);

        /* check the point is on the face plane */
        vec_t dist = plane.distance_to(p1);
        if (dist < -ON_EPSILON || dist > ON_EPSILON)
            LogPrint("WARNING: Line {}: Point ({:.3} {:.3} {:.3}) off plane by {:2.4}\n", sourceface.linenum, p1[0],
                p1[1], p1[2], dist);

        /* check the edge isn't degenerate */
        qvec3d edgevec = p2 - p1;
        vec_t length = qv::length(edgevec);
        if (length < ON_EPSILON) {
            LogPrint("WARNING: Line {}: Healing degenerate edge ({}) at ({:.3f} {:.3} {:.3})\n", sourceface.linenum,
                length, p1[0], p1[1], p1[2]);
            for (size_t j = i + 1; j < face->w.size(); j++)
                VectorCopy(face->w[j], face->w[j - 1]);
            face->w.resize(face->w.size() - 1);
            CheckFace(face, sourceface);
            break;
        }

        qvec3d edgenormal = qv::normalize(qv::cross(facenormal, edgevec));
        vec_t edgedist = qv::dot(p1, edgenormal);
        edgedist += ON_EPSILON;

        /* all other points must be on front side */
        for (size_t j = 0; j < face->w.size(); j++) {
            if (j == i)
                continue;
            dist = qv::dot(face->w[j], edgenormal);
            if (dist > edgedist)
                FError("line {}: Found a non-convex face (error size {}, point: {})\n", sourceface.linenum,
                    dist - edgedist, face->w[j]);
        }
    }
}

//===========================================================================

static bool NormalizePlane(qbsp_plane_t &p, bool flip = true)
{
    for (size_t i = 0; i < 3; i++) {
        if (p.normal[i] == 1.0) {
            p.normal[(i + 1) % 3] = 0;
            p.normal[(i + 2) % 3] = 0;
            p.type = PLANE_X + i;
            return 0; /* no flip */
        }
        if (p.normal[i] == -1.0) {
            if (flip) {
                p.normal[i] = 1.0;
                p.dist = -p.dist;
            }
            p.normal[(i + 1) % 3] = 0;
            p.normal[(i + 2) % 3] = 0;
            p.type = PLANE_X + i;
            return 1; /* plane flipped */
        }
    }

    vec_t ax = fabs(p.normal[0]);
    vec_t ay = fabs(p.normal[1]);
    vec_t az = fabs(p.normal[2]);

    if (ax >= ay && ax >= az)
        p.type = PLANE_ANYX;
    else if (ay >= ax && ay >= az)
        p.type = PLANE_ANYY;
    else
        p.type = PLANE_ANYZ;

    if (flip && p.normal[p.type - PLANE_ANYX] < 0) {
        p = -p;
        return true; /* plane flipped */
    }

    return false; /* no flip */
}

/* Plane Hashing */

inline int plane_hash_fn(const qplane3d &p)
{
    return Q_rint(fabs(p.dist));
}

static void PlaneHash_Add(const qplane3d &p, int index)
{
    const int hash = plane_hash_fn(p);
    map.planehash[hash].push_back(index);
}

/*
 * NewPlane
 * - Returns a global plane number and the side that will be the front
 */
static int NewPlane(const qplane3d &plane, int *side)
{
    vec_t len = qv::length(plane.normal);

    if (len < 1 - ON_EPSILON || len > 1 + ON_EPSILON)
        FError("invalid normal (vector length {:.4})", len);

    size_t index = map.planes.size();
    qbsp_plane_t &added_plane = map.planes.emplace_back(qbsp_plane_t { plane });

    bool out_flipped = NormalizePlane(added_plane, side != nullptr);

    if (side) {
        *side = out_flipped ? SIDE_BACK : SIDE_FRONT;
    }

    PlaneHash_Add(added_plane, index);
    return index;
}

/*
 * FindPlane
 * - Returns a global plane number and the side that will be the front
 * - if `side` is null, only an exact match will be fetched.
 */
int FindPlane(const qplane3d &plane, int *side)
{
    for (int i : map.planehash[plane_hash_fn(plane)]) {
        const qbsp_plane_t &p = map.planes.at(i);
        if (qv::epsilonEqual(p, plane)) {
            if (side) {
                *side = SIDE_FRONT;
            }
            return i;
        } else if (side && qv::epsilonEqual(-p, plane)) {
            *side = SIDE_BACK;
            return i;
        }
    }
    return NewPlane(plane, side);
}

/*
=============================================================================

                        TURN BRUSHES INTO GROUPS OF FACES

=============================================================================
*/

/*
=================
FindTargetEntity
=================
*/
static const mapentity_t *FindTargetEntity(const char *target)
{
    for (const auto &entity : map.entities) {
        const char *name = ValueForKey(&entity, "targetname");
        if (!Q_strcasecmp(target, name))
            return &entity;
    }

    return nullptr;
}

/*
=================
FixRotateOrigin
=================
*/
void FixRotateOrigin(mapentity_t *entity)
{
    const char *search = ValueForKey(entity, "target");
    const mapentity_t *target = nullptr;

    if (search[0])
        target = FindTargetEntity(search);

    qvec3d offset;

    if (target) {
        GetVectorForKey(target, "origin", offset);
    } else {
        search = ValueForKey(entity, "classname");
        LogPrint("WARNING: No target for rotation entity \"{}\"", search);
        offset = {};
    }

    SetKeyValue(entity, "origin", qv::to_string(offset).c_str());
}

static bool DiscardHintSkipFace_Q1(const int hullnum, const hullbrush_t *hullbrush, const mtexinfo_t &texinfo)
{
    // anything texname other than "hint" in a hint brush is treated as "hintskip", and discarded
    return !string_iequals(map.miptexTextureName(texinfo.miptex), "hint");
}

static bool DiscardHintSkipFace_Q2(const int hullnum, const hullbrush_t *hullbrush, const mtexinfo_t &texinfo)
{
    // any face in a hint brush that isn't HINT are treated as "hintskip", and discarded
    return !(texinfo.flags.native & Q2_SURF_HINT);
}

/*
=================
CreateBrushFaces
=================
*/
static face_t *CreateBrushFaces(const mapentity_t *src, hullbrush_t *hullbrush, const int hullnum,
                                const rotation_t rottype = rotation_t::none, const qvec3d &rotate_offset = {})
{
    vec_t r;
    face_t *f;
    std::optional<winding_t> w;
    qbsp_plane_t plane;
    face_t *facelist = NULL;
    qvec3d point;
    vec_t max, min;

    min = VECT_MAX;
    max = -VECT_MAX;

    hullbrush->bounds = {};

    auto DiscardHintSkipFace =
        (options.target_game->id == GAME_QUAKE_II) ? DiscardHintSkipFace_Q2 : DiscardHintSkipFace_Q1;

    for (auto &mapface : hullbrush->faces) {
        if (hullnum <= 0 && hullbrush->contents.is_hint()) {
            /* Don't generate hintskip faces */
            const mtexinfo_t &texinfo = map.mtexinfos.at(mapface.texinfo);

            if (DiscardHintSkipFace(hullnum, hullbrush, texinfo))
                continue;
        }

        w = BaseWindingForPlane(mapface.plane);

        for (auto &mapface2 : hullbrush->faces) {
            if (&mapface == &mapface2)
                continue;

            // flip the plane, because we want to keep the back side
            plane = -mapface2.plane;

            w = w->clip(plane, ON_EPSILON, false)[SIDE_FRONT];
        }

        if (!w)
            continue; // overconstrained plane

        // this face is a keeper
        f = new face_t{};
        f->planenum = PLANENUM_LEAF;

        if (w->size() > MAXEDGES)
            FError("face->numpoints > MAXEDGES ({}), source face on line {}", MAXEDGES, mapface.linenum);

        f->w.resize(w->size());

        for (size_t j = 0; j < w->size(); j++) {
            for (size_t k = 0; k < 3; k++) {
                point[k] = w->at(j)[k] - rotate_offset[k];
                r = Q_rint(point[k]);
                if (fabs(point[k] - r) < ZERO_EPSILON)
                    f->w[j][k] = r;
                else
                    f->w[j][k] = point[k];

                if (f->w[j][k] < min)
                    min = f->w[j][k];
                if (f->w[j][k] > max)
                    max = f->w[j][k];
            }

            hullbrush->bounds += f->w[j];
        }

        // account for texture offset, from txqbsp-xt
        if (options.fixRotateObjTexture) {
            mtexinfo_t texInfoNew = map.mtexinfos.at(mapface.texinfo);
            texInfoNew.outputnum = std::nullopt;

            texInfoNew.vecs.at(0, 3) += qv::dot(rotate_offset, texInfoNew.vecs.row(0).xyz());
            texInfoNew.vecs.at(1, 3) += qv::dot(rotate_offset, texInfoNew.vecs.row(1).xyz());

            mapface.texinfo = FindTexinfo(texInfoNew);
        }

        VectorCopy(mapface.plane.normal, plane.normal);
        VectorScale(mapface.plane.normal, mapface.plane.dist, point);
        VectorSubtract(point, rotate_offset, point);
        plane.dist = qv::dot(plane.normal, point);

        f->texinfo = hullnum > 0 ? 0 : mapface.texinfo;
        f->planenum = FindPlane(plane, &f->planeside);
        f->next = facelist;
        facelist = f;
        CheckFace(f, mapface);
        UpdateFaceSphere(f);
    }

    // Rotatable objects must have a bounding box big enough to
    // account for all its rotations

    // if -wrbrushes is in use, don't do this for the clipping hulls because it depends on having
    // the actual non-hacked bbox (it doesn't write axial planes).

    // Hexen2 also doesn't want the bbox expansion, it's handled in engine (see: SV_LinkEdict)

    // Only do this for hipnotic rotation. For origin brushes in Quake, it breaks some of their
    // uses (e.g. func_train). This means it's up to the mapper to expand the model bounds with
    // clip brushes if they're going to rotate a model in vanilla Quake and not use hipnotic rotation.
    // The idea behind the bounds expansion was to avoid incorrect vis culling (AFAIK).
    const bool shouldExpand = (rotate_offset[0] != 0.0 || rotate_offset[1] != 0.0 || rotate_offset[2] != 0.0) &&
                              rottype == rotation_t::hipnotic &&
                              (hullnum >= 0) // hullnum < 0 corresponds to -wrbrushes clipping hulls
                              && options.target_game->id != GAME_HEXEN_II; // never do this in Hexen 2

    if (shouldExpand) {
        vec_t delta = std::max(fabs(max), fabs(min));
        hullbrush->bounds = {-delta, delta};
    }

    return facelist;
}

/*
=================
FreeBrushFaces
=================
*/
static void FreeBrushFaces(face_t *facelist)
{
    face_t *face, *next;

    for (face = facelist; face; face = next) {
        next = face->next;
        delete face;
    }
}

/*
=====================
FreeBrushes
=====================
*/
void FreeBrushes(mapentity_t *ent)
{
    brush_t *brush, *next;

    for (brush = ent->brushes; brush; brush = next) {
        next = brush->next;
        FreeBrush(brush);
    }
    ent->brushes = nullptr;
}

/*
=====================
FreeBrush
=====================
*/
void FreeBrush(brush_t *brush)
{
    FreeBrushFaces(brush->faces);
    delete brush;
}

/*
==============================================================================

BEVELED CLIPPING HULL GENERATION

This is done by brute force, and could easily get a lot faster if anyone cares.
==============================================================================
*/

/*
============
AddBrushPlane
=============
*/
static void AddBrushPlane(hullbrush_t *hullbrush, const qplane3d &plane)
{
    vec_t len = qv::length(plane.normal);

    if (len < 1.0 - NORMAL_EPSILON || len > 1.0 + NORMAL_EPSILON)
        FError("invalid normal (vector length {:.4})", len);

    for (auto &mapface : hullbrush->faces) {
        if (qv::epsilonEqual(mapface.plane.normal, plane.normal, EQUAL_EPSILON) &&
            fabs(mapface.plane.dist - plane.dist) < ON_EPSILON)
            return;
    }

    if (hullbrush->faces.size() == MAX_FACES)
        FError(
            "brush->faces >= MAX_FACES ({}), source brush on line {}", MAX_FACES, hullbrush->srcbrush->face(0).linenum);

    mapface_t &mapface = hullbrush->faces.emplace_back();
    mapface.plane = { plane };
    mapface.texinfo = 0;
}

/*
============
TestAddPlane

Adds the given plane to the brush description if all of the original brush
vertexes can be put on the front side
=============
*/
static void TestAddPlane(hullbrush_t *hullbrush, qplane3d &plane)
{
    vec_t d;
    int points_front, points_back;

    /* see if the plane has already been added */
    for (auto &mapface : hullbrush->faces) {
        if (qv::epsilonEqual(plane, mapface.plane))
            return;
        if (qv::epsilonEqual(-plane, mapface.plane))
            return;
    }

    /* check all the corner points */
    points_front = 0;
    points_back = 0;

    for (auto &corner : hullbrush->corners) {
        d = plane.distance_to(corner);
        if (d < -ON_EPSILON) {
            if (points_front)
                return;
            points_back = 1;
        } else if (d > ON_EPSILON) {
            if (points_back)
                return;
            points_front = 1;
        }
    }

    // the plane is a seperator
    if (points_front) {
        plane = -plane;
    }

    AddBrushPlane(hullbrush, plane);
}

/*
============
AddHullPoint

Doesn't add if duplicated
=============
*/
static int AddHullPoint(hullbrush_t *hullbrush, const qvec3d &p, const aabb3d &hull_size)
{
    for (auto &pt : hullbrush->points)
        if (qv::epsilonEqual(p, pt, EQUAL_EPSILON))
            return &pt - hullbrush->points.data();

    if (hullbrush->points.size() == MAX_HULL_POINTS)
        FError("hullbrush->numpoints == MAX_HULL_POINTS ({}), "
               "source brush on line {}",
            MAX_HULL_POINTS, hullbrush->srcbrush->face(0).linenum);

    int i = hullbrush->points.size();
    hullbrush->points.emplace_back(p);

    for (size_t x = 0; x < 2; x++)
        for (size_t y = 0; y < 2; y++)
            for (size_t z = 0; z < 2; z++) {
                hullbrush->corners.emplace_back(
                    p[0] + hull_size[x][0],
                    p[1] + hull_size[y][1],
                    p[2] + hull_size[z][2]);
            }

    return i;
}

/*
============
AddHullEdge

Creates all of the hull planes around the given edge, if not done allready
=============
*/
static void AddHullEdge(hullbrush_t *hullbrush, const qvec3d &p1, const qvec3d &p2, const aabb3d &hull_size)
{
    int pt1, pt2;
    int a, b, c, d, e;
    qplane3d plane;
    vec_t length;

    pt1 = AddHullPoint(hullbrush, p1, hull_size);
    pt2 = AddHullPoint(hullbrush, p2, hull_size);

    for (auto &edge : hullbrush->edges)
        if ((edge == std::make_tuple(pt1, pt2)) ||
            (edge == std::make_tuple(pt2, pt1)))
            return;

    if (hullbrush->edges.size() == MAX_HULL_EDGES)
        FError("hullbrush->numedges == MAX_HULL_EDGES ({}), "
               "source brush on line {}",
            MAX_HULL_EDGES, hullbrush->srcbrush->face(0).linenum);

    hullbrush->edges.emplace_back(pt1, pt2);

    qvec3d edgevec = p1 - p2;
    VectorNormalize(edgevec);

    for (a = 0; a < 3; a++) {
        b = (a + 1) % 3;
        c = (a + 2) % 3;

        qvec3d planevec;
        planevec[a] = 1;
        planevec[b] = 0;
        planevec[c] = 0;
        plane.normal = qv::cross(planevec, edgevec);
        length = qv::normalizeInPlace(plane.normal);

        /* If this edge is almost parallel to the hull edge, skip it. */
        if (length < ANGLEEPSILON)
            continue;

        for (d = 0; d <= 1; d++) {
            for (e = 0; e <= 1; e++) {
                qvec3d planeorg = p1;
                planeorg[b] += hull_size[d][b];
                planeorg[c] += hull_size[e][c];
                plane.dist = qv::dot(planeorg, plane.normal);
                TestAddPlane(hullbrush, plane);
            }
        }
    }
}

/*
============
ExpandBrush
=============
*/
static void ExpandBrush(hullbrush_t *hullbrush, const aabb3d &hull_size, const face_t *facelist)
{
    int x, s;
    const face_t *f;
    qbsp_plane_t plane;
    int cBevEdge = 0;

    hullbrush->points.clear();
    hullbrush->corners.clear();
    hullbrush->edges.clear();

    // create all the hull points
    for (f = facelist; f; f = f->next)
        for (size_t i = 0; i < f->w.size(); i++) {
            AddHullPoint(hullbrush, f->w[i], hull_size);
            cBevEdge++;
        }

    // expand all of the planes
    for (auto &mapface : hullbrush->faces) {
        if (mapface.flags.no_expand)
            continue;
        qvec3d corner { };
        for (x = 0; x < 3; x++) {
            if (mapface.plane.normal[x] > 0)
                corner[x] = hull_size[1][x];
            else if (mapface.plane.normal[x] < 0)
                corner[x] = hull_size[0][x];
        }
        mapface.plane.dist += qv::dot(corner, mapface.plane.normal);
    }

    // add any axis planes not contained in the brush to bevel off corners
    for (x = 0; x < 3; x++)
        for (s = -1; s <= 1; s += 2) {
            // add the plane
            plane.normal = {};
            plane.normal[x] = (vec_t)s;
            if (s == -1)
                plane.dist = -hullbrush->bounds.mins()[x] + -hull_size[0][x];
            else
                plane.dist = hullbrush->bounds.maxs()[x] + hull_size[1][x];
            AddBrushPlane(hullbrush, plane);
        }

    // add all of the edge bevels
    for (f = facelist; f; f = f->next)
        for (size_t i = 0; i < f->w.size(); i++)
            AddHullEdge(hullbrush, f->w[i], f->w[(i + 1) % f->w.size()], hull_size);
}

//============================================================================

static const int DetailFlag = (1 << 27);

static bool Brush_IsDetail(const mapbrush_t *mapbrush)
{
    const mapface_t &mapface = mapbrush->face(0);

    if ((mapface.contents & DetailFlag) == DetailFlag) {
        return true;
    }
    return false;
}

static contentflags_t Brush_GetContents_Q1(const mapbrush_t *mapbrush)
{
    const char *texname;

    // check for strong content indicators
    for (int i = 0; i < mapbrush->numfaces; i++) {
        const mapface_t &mapface = mapbrush->face(i);
        const mtexinfo_t &texinfo = map.mtexinfos.at(mapface.texinfo);
        texname = map.miptexTextureName(texinfo.miptex).c_str();

        if (!Q_strcasecmp(texname, "origin"))
            return options.target_game->create_extended_contents(CFLAGS_ORIGIN);
        else if (!Q_strcasecmp(texname, "hint"))
            return options.target_game->create_extended_contents(CFLAGS_HINT);
        else if (!Q_strcasecmp(texname, "clip"))
            return options.target_game->create_extended_contents(CFLAGS_CLIP);
        else if (texname[0] == '*') {
            if (!Q_strncasecmp(texname + 1, "lava", 4))
                return options.target_game->create_liquid_contents(CONTENTS_LAVA);
            else if (!Q_strncasecmp(texname + 1, "slime", 5))
                return options.target_game->create_liquid_contents(CONTENTS_SLIME);
            else
                return options.target_game->create_liquid_contents(CONTENTS_WATER);
        } else if (!Q_strncasecmp(texname, "sky", 3))
            return options.target_game->create_sky_contents();
    }

    // and anything else is assumed to be a regular solid.
    return options.target_game->create_solid_contents();
}

static contentflags_t Brush_GetContents_Q2(const mapbrush_t *mapbrush)
{
    bool is_trans = false;
    bool is_hint = false;
    contentflags_t contents = {mapbrush->face(0).contents};

    for (int i = 0; i < mapbrush->numfaces; i++) {
        const mapface_t &mapface = mapbrush->face(i);
        const mtexinfo_t &texinfo = map.mtexinfos.at(mapface.texinfo);

        if (texinfo.flags.is_skip) {
            continue;
        }

        if (!is_trans && (texinfo.flags.native & (Q2_SURF_TRANS33 | Q2_SURF_TRANS66))) {
            is_trans = true;
        }

        if (!is_hint && (texinfo.flags.native & Q2_SURF_HINT)) {
            is_hint = true;
        }

        if (mapface.contents != contents.native) {
            LogPrint("mixed face contents ({} != {} at line {})\n",
                contentflags_t{mapface.contents}.to_string(options.target_game),
                contents.to_string(options.target_game), mapface.linenum);
            break;
        }
    }

    // if any side is translucent, mark the contents
    // and change solid to window
    if (is_trans) {
        contents.native |= Q2_CONTENTS_TRANSLUCENT;
        if (contents.native & Q2_CONTENTS_SOLID) {
            contents.native = (contents.native & ~Q2_CONTENTS_SOLID) | Q2_CONTENTS_WINDOW;
        }
    }

    // add extended flags that we may need
    if (contents.native & Q2_CONTENTS_DETAIL) {
        contents.extended |= CFLAGS_DETAIL;
    }

    if (contents.native & (Q2_CONTENTS_MONSTERCLIP | Q2_CONTENTS_PLAYERCLIP)) {
        contents.extended |= CFLAGS_CLIP;
    }

    if (contents.native & Q2_CONTENTS_ORIGIN) {
        contents.extended |= CFLAGS_ORIGIN;
    }

    if (contents.native & Q2_CONTENTS_MIST) {
        contents.extended |= CFLAGS_DETAIL_ILLUSIONARY;
    }

    if (is_hint) {
        contents.extended |= CFLAGS_HINT;
    }

    // FIXME: this is a bit of a hack, but this is because clip
    // and liquids and stuff are already handled *like* detail by
    // the compiler.
    if (contents.extended & CFLAGS_DETAIL) {
        if (!(contents.native & Q2_CONTENTS_SOLID)) {
            contents.extended &= ~CFLAGS_DETAIL;
        }
    }

    Q_assert(contents.is_valid(options.target_game, false));

    return contents;
}

/*
===============
LoadBrush

Converts a mapbrush to a bsp brush
===============
*/
brush_t *LoadBrush(const mapentity_t *src, const mapbrush_t *mapbrush, const contentflags_t &contents,
    const qvec3d &rotate_offset, const rotation_t rottype, const int hullnum)
{
    hullbrush_t hullbrush;
    brush_t *brush;
    face_t *facelist;

    // create the faces

    hullbrush.linenum = mapbrush->face(0).linenum;
    if (mapbrush->numfaces > MAX_FACES)
        FError("brush->faces >= MAX_FACES ({}), source brush on line {}", MAX_FACES, hullbrush.linenum);

    hullbrush.contents = contents;
    hullbrush.srcbrush = mapbrush;
    hullbrush.faces.reserve(mapbrush->numfaces);
    for (int i = 0; i < mapbrush->numfaces; i++)
        hullbrush.faces.emplace_back(mapbrush->face(i));

    if (hullnum <= 0) {
        // for hull 0 or BSPX -wrbrushes collision, apply the rotation offset now
        facelist = CreateBrushFaces(src, &hullbrush, hullnum, rottype, rotate_offset);
    } else {
        // for Quake-style clipping hulls, don't apply rotation offset yet..
        // it will be applied below
        facelist = CreateBrushFaces(src, &hullbrush, hullnum);
    }

    if (!facelist) {
        LogPrint("WARNING: Couldn't create brush faces\n");
        LogPrint("^ brush at line {} of .map file\n", hullbrush.linenum);
        return NULL;
    }

    if (hullnum > 0) {
        auto &hulls = options.target_game->get_hull_sizes();
        Q_assert(hullnum < hulls.size());
        ExpandBrush(&hullbrush, *(hulls.begin() + hullnum), facelist);
        FreeBrushFaces(facelist);
        facelist = CreateBrushFaces(src, &hullbrush, hullnum, rottype, rotate_offset);
    }

    // create the brush
    brush = new brush_t{};

    brush->contents = contents;
    brush->faces = facelist;
    brush->bounds = hullbrush.bounds;

    return brush;
}

//=============================================================================

static brush_t *Brush_ListTail(brush_t *brush)
{
    if (brush == nullptr) {
        return nullptr;
    }

    while (brush->next != nullptr) {
        brush = brush->next;
    }

    Q_assert(brush->next == nullptr);
    return brush;
}

int Brush_ListCountWithCFlags(const brush_t *brush, int cflags)
{
    int cnt = 0;
    for (const brush_t *b = brush; b; b = b->next) {
        if (cflags == (b->contents.extended & cflags))
            cnt++;
    }
    return cnt;
}

int Brush_ListCount(const brush_t *brush)
{
    return Brush_ListCountWithCFlags(brush, 0);
}

static int FaceListCount(const face_t *facelist)
{
    if (facelist)
        return 1 + FaceListCount(facelist->next);
    else
        return 0;
}

int Brush_NumFaces(const brush_t *brush)
{
    return FaceListCount(brush->faces);
}

void Entity_SortBrushes(mapentity_t *dst)
{
    Q_assert(dst->brushes == nullptr);

    brush_t **nextLink = &dst->brushes;

    if (dst->detail_illusionary) {
        brush_t *last = Brush_ListTail(dst->detail_illusionary);
        *nextLink = dst->detail_illusionary;
        nextLink = &last->next;
    }
    if (dst->liquid) {
        brush_t *last = Brush_ListTail(dst->liquid);
        *nextLink = dst->liquid;
        nextLink = &last->next;
    }
    if (dst->detail_fence) {
        brush_t *last = Brush_ListTail(dst->detail_fence);
        *nextLink = dst->detail_fence;
        nextLink = &last->next;
    }
    if (dst->detail) {
        brush_t *last = Brush_ListTail(dst->detail);
        *nextLink = dst->detail;
        nextLink = &last->next;
    }
    if (dst->sky) {
        brush_t *last = Brush_ListTail(dst->sky);
        *nextLink = dst->sky;
        nextLink = &last->next;
    }
    if (dst->solid) {
        brush_t *last = Brush_ListTail(dst->solid);
        *nextLink = dst->solid;
        nextLink = &last->next;
    }
}

/*
============
Brush_LoadEntity

hullnum -1 should contain ALL brushes. (used by BSPX_CreateBrushList())
hullnum 0 does not contain clip brushes.
============
*/
void Brush_LoadEntity(mapentity_t *dst, const mapentity_t *src, const int hullnum)
{
    const char *classname;
    const mapbrush_t *mapbrush;
    qvec3d rotate_offset{};
    int i;
    int lmshift;
    bool all_detail, all_detail_fence, all_detail_illusionary;

    /*
     * The brush list needs to be ordered (lowest to highest priority):
     * - detail_illusionary (which is saved as empty)
     * - liquid
     * - detail_fence
     * - detail (which is solid)
     * - sky
     * - solid
     */

    classname = ValueForKey(src, "classname");

    /* Origin brush support */
    rotation_t rottype = rotation_t::none;

    // TODO: move to game
    auto Brush_GetContents = (options.target_game->id == GAME_QUAKE_II) ? Brush_GetContents_Q2 : Brush_GetContents_Q1;

    for (int i = 0; i < src->nummapbrushes; i++) {
        const mapbrush_t *mapbrush = &src->mapbrush(i);
        const contentflags_t contents = Brush_GetContents(mapbrush);
        if (contents.is_origin()) {
            if (dst == pWorldEnt()) {
                LogPrint("WARNING: Ignoring origin brush in worldspawn\n");
                continue;
            }

            brush_t *brush = LoadBrush(src, mapbrush, contents, {}, rotation_t::none, 0);
            if (brush) {
                rotate_offset = brush->bounds.centroid();

                SetKeyValue(dst, "origin", qv::to_string(rotate_offset).c_str());

                rottype = rotation_t::origin_brush;

                FreeBrush(brush);
            }
        }
    }

    /* Hipnotic rotation */
    if (rottype == rotation_t::none) {
        if (!strncmp(classname, "rotate_", 7)) {
            FixRotateOrigin(dst);
            GetVectorForKey(dst, "origin", rotate_offset);
            rottype = rotation_t::hipnotic;
        }
    }

    /* If the source entity is func_detail, set the content flag */
    all_detail = false;
    if (!Q_strcasecmp(classname, "func_detail") && !options.fNodetail) {
        all_detail = true;
    }

    all_detail_fence = false;
    if ((!Q_strcasecmp(classname, "func_detail_fence") || !Q_strcasecmp(classname, "func_detail_wall")) &&
        !options.fNodetail) {
        all_detail_fence = true;
    }

    all_detail_illusionary = false;
    if (!Q_strcasecmp(classname, "func_detail_illusionary") && !options.fNodetail) {
        all_detail_illusionary = true;
    }

    /* entities with custom lmscales are important for the qbsp to know about */
    i = 16 * atof(ValueForKey(src, "_lmscale"));
    if (!i)
        i = 16; // if 0, pick a suitable default
    lmshift = 0;
    while (i > 1) {
        lmshift++; // only allow power-of-two scales
        i /= 2;
    }

    /* _mirrorinside key (for func_water etc.) */
    const bool mirrorinside = !!atoi(ValueForKey(src, "_mirrorinside"));

    /* _noclipfaces */
    const bool noclipfaces = !!atoi(ValueForKey(src, "_noclipfaces"));

    const bool func_illusionary_visblocker = (0 == Q_strcasecmp(classname, "func_illusionary_visblocker"));

    // _omitbrushes 1 just discards all brushes in the entity.
    // could be useful for geometry guides, selective compilation, etc.
    if (atoi(ValueForKey(src, "_omitbrushes")))
        return;

    // _omitbrushes 1 just discards all brushes in the entity.
    // could be useful for geometry guides, selective compilation, etc.
    if (atoi(ValueForKey(src, "_omitbrushes")))
        return;

    for (i = 0; i < src->nummapbrushes; i++, mapbrush++) {
        mapbrush = &src->mapbrush(i);
        contentflags_t contents = Brush_GetContents(mapbrush);

        // per-brush settings
        bool detail = Brush_IsDetail(mapbrush);
        bool detail_illusionary = false;
        bool detail_fence = false;

        // inherit the per-entity settings
        detail |= all_detail;
        detail_illusionary |= all_detail_illusionary;
        detail_fence |= all_detail_fence;

        /* "origin" brushes always discarded */
        if (contents.is_origin())
            continue;

        /* -omitdetail option omits all types of detail */
        if (options.fOmitDetail && detail)
            continue;
        if ((options.fOmitDetail || options.fOmitDetailIllusionary) && detail_illusionary)
            continue;
        if ((options.fOmitDetail || options.fOmitDetailFence) && detail_fence)
            continue;

        /* turn solid brushes into detail, if we're in hull0 */
        if (hullnum <= 0 && contents.is_solid(options.target_game)) {
            if (detail) {
                contents = options.target_game->create_extended_contents(CFLAGS_DETAIL);
            } else if (detail_illusionary) {
                contents = options.target_game->create_extended_contents(CFLAGS_DETAIL_ILLUSIONARY);
            } else if (detail_fence) {
                contents = options.target_game->create_extended_contents(CFLAGS_DETAIL_FENCE);
            }
        }

        /* func_detail_illusionary don't exist in the collision hull
         * (or bspx export) */
        if ((options.target_game->id != GAME_QUAKE_II && hullnum) && detail_illusionary) {
            continue;
        }

        /*
         * "clip" brushes don't show up in the draw hull, but we still want to
         * include them in the model bounds so collision detection works
         * correctly.
         */
        if (contents.is_clip()) {
            if (hullnum == 0) {
                brush_t *brush = LoadBrush(src, mapbrush, contents, rotate_offset, rottype, hullnum);
                if (brush) {
                    dst->bounds += brush->bounds;
                    FreeBrush(brush);
                }
                continue;
            }
            // for hull1, 2, etc., convert clip to CONTENTS_SOLID
            if (hullnum > 0) {
                contents = options.target_game->create_solid_contents();
            }
            // if hullnum is -1 (bspx brush export), leave it as CONTENTS_CLIP
        }

        /* "hint" brushes don't affect the collision hulls */
        if (contents.is_hint()) {
            if (hullnum > 0)
                continue;
            contents = options.target_game->create_empty_contents();
        }

        /* entities never use water merging */
        if (dst != pWorldEnt()) {
            contents = options.target_game->create_solid_contents();
        }

        /* Hack to turn bmodels with "_mirrorinside" into func_detail_fence in hull 0.
           this is to allow "_mirrorinside" to work on func_illusionary, func_wall, etc.
           Otherwise they would be CONTENTS_SOLID and the inside faces would be deleted.

           It's CONTENTS_DETAIL_FENCE because this gets mapped to CONTENTS_SOLID just
           before writing the bsp, and bmodels normally have CONTENTS_SOLID as their
           contents type.
         */
        if (dst != pWorldEnt() && hullnum <= 0 && mirrorinside) {
            contents = options.target_game->create_extended_contents(CFLAGS_DETAIL_FENCE);
        }

        /* nonsolid brushes don't show up in clipping hulls */
        if (hullnum > 0 && !contents.is_solid(options.target_game) && !contents.is_sky(options.target_game))
            continue;

        /* sky brushes are solid in the collision hulls */
        if (hullnum > 0 && contents.is_sky(options.target_game))
            contents = options.target_game->create_solid_contents();

        // apply extended flags
        if (mirrorinside) {
            contents.extended |= CFLAGS_BMODEL_MIRROR_INSIDE;
        }
        if (noclipfaces) {
            contents.extended |= CFLAGS_NO_CLIPPING_SAME_TYPE;
        }
        if (func_illusionary_visblocker) {
            contents.extended |= CFLAGS_ILLUSIONARY_VISBLOCKER;
        }

        brush_t *brush = LoadBrush(src, mapbrush, contents, rotate_offset, rottype, hullnum);
        if (!brush)
            continue;

        dst->numbrushes++;
        brush->lmshift = lmshift;

        if (brush->contents.is_solid(options.target_game)) {
            brush->next = dst->solid;
            dst->solid = brush;
        } else if (brush->contents.is_sky(options.target_game)) {
            brush->next = dst->sky;
            dst->sky = brush;
        } else if (brush->contents.is_detail(CFLAGS_DETAIL)) {
            brush->next = dst->detail;
            dst->detail = brush;
        } else if (brush->contents.is_detail(CFLAGS_DETAIL_ILLUSIONARY)) {
            brush->next = dst->detail_illusionary;
            dst->detail_illusionary = brush;
        } else if (brush->contents.is_detail(CFLAGS_DETAIL_FENCE)) {
            brush->next = dst->detail_fence;
            dst->detail_fence = brush;
        } else {
            brush->next = dst->liquid;
            dst->liquid = brush;
        }

        dst->bounds += brush->bounds;

        LogPercent(i + 1, src->nummapbrushes);
    }
}
