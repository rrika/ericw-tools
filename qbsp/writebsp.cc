/*
    Copyright (C) 1996-1997  Id Software, Inc.
    Copyright (C) 1997       Greg Lewis

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
// writebsp.c

#include <qbsp/qbsp.hh>
#include <qbsp/wad.hh>

#include <vector>
#include <algorithm>
#include <cstdint>

static contentflags_t RemapContentsForExport(const contentflags_t &content)
{
    if (content.extended & CFLAGS_DETAIL_FENCE) {
        /*
         * This is for func_detail_wall.. we want to write a solid leaf that has faces,
         * because it may be possible to see inside (fence textures).
         *
         * Normally solid leafs are not written and just referenced as leaf 0.
         */
        return options.target_game->create_solid_contents();
    }

    return content;
}

/**
 * Returns the output plane number
 */
size_t ExportMapPlane(size_t planenum)
{
    qbsp_plane_t &plane = map.planes.at(planenum);

    if (plane.outputplanenum.has_value())
        return plane.outputplanenum.value(); // already output.

    const size_t newIndex = map.bsp.dplanes.size();
    dplane_t &dplane = map.bsp.dplanes.emplace_back();
    dplane.normal[0] = plane.normal[0];
    dplane.normal[1] = plane.normal[1];
    dplane.normal[2] = plane.normal[2];
    dplane.dist = plane.dist;
    dplane.type = plane.type;

    plane.outputplanenum = newIndex;
    return newIndex;
}

size_t ExportMapTexinfo(size_t texinfonum)
{
    mtexinfo_t &src = map.mtexinfos.at(texinfonum);

    if (src.outputnum.has_value())
        return src.outputnum.value();

    // this will be the index of the exported texinfo in the BSP lump
    const size_t i = map.bsp.texinfo.size();

    gtexinfo_t &dest = map.bsp.texinfo.emplace_back();

    // make sure we don't write any non-native flags.
    // e.g. Quake only accepts 0 or TEX_SPECIAL.
    if (!src.flags.is_valid(options.target_game)) {
        FError("Internal error: Texinfo {} has invalid surface flags {}", texinfonum, src.flags.native);
    }

    dest.flags = src.flags;
    dest.miptex = src.miptex;
    dest.vecs = src.vecs;
    strcpy(dest.texture.data(), map.texinfoTextureName(texinfonum).c_str());
    dest.value = map.miptex[src.miptex].value;

    src.outputnum = i;

    return i;
}

//===========================================================================

/*
==================
ExportClipNodes
==================
*/
static size_t ExportClipNodes(mapentity_t *entity, node_t *node)
{
    face_t *face, *next;

    // FIXME: free more stuff?
    if (node->planenum == PLANENUM_LEAF) {
        int contents = node->contents.native;
        delete node;
        return contents;
    }

    /* emit a clipnode */
    const size_t nodenum = map.bsp.dclipnodes.size();
    map.bsp.dclipnodes.emplace_back();

    const int child0 = ExportClipNodes(entity, node->children[0]);
    const int child1 = ExportClipNodes(entity, node->children[1]);

    // Careful not to modify the vector while using this clipnode pointer
    bsp2_dclipnode_t &clipnode = map.bsp.dclipnodes[nodenum];
    clipnode.planenum = ExportMapPlane(node->planenum);
    clipnode.children[0] = child0;
    clipnode.children[1] = child1;

    for (face = node->faces; face; face = next) {
        next = face->next;
        delete face;
    }

    delete node;
    return nodenum;
}

/*
==================
ExportClipNodes

Called after the clipping hull is completed.  Generates a disk format
representation and frees the original memory.

This gets real ugly.  Gets called twice per entity, once for each clip hull.
First time just store away data, second time fix up reference points to
accomodate new data interleaved with old.
==================
*/
void ExportClipNodes(mapentity_t *entity, node_t *nodes, const int hullnum)
{
    auto &model = map.bsp.dmodels.at(entity->outputmodelnumber.value());
    model.headnode[hullnum] = ExportClipNodes(entity, nodes);
}

//===========================================================================

/*
==================
ExportLeaf
==================
*/
static void ExportLeaf(mapentity_t *entity, node_t *node)
{
    mleaf_t &dleaf = map.bsp.dleafs.emplace_back();

    const contentflags_t remapped = RemapContentsForExport(node->contents);

    if (!remapped.is_valid(options.target_game, false)) {
        FError("Internal error: On leaf {}, tried to save invalid contents type {}", map.bsp.dleafs.size() - 1, remapped.to_string(options.target_game));
    }

    dleaf.contents = remapped.native;

    /*
     * write bounding box info
     */
    for (int32_t i = 0; i < 3; ++i) {
        dleaf.mins[i] = floor(node->bounds.mins()[i]);
        dleaf.maxs[i] = ceil(node->bounds.maxs()[i]);
    }

    dleaf.visofs = -1; // no vis info yet

    // write the marksurfaces
    dleaf.firstmarksurface = static_cast<int>(map.bsp.dleaffaces.size());

    for (face_t **markfaces = node->markfaces; *markfaces; markfaces++) {
        face_t *face = *markfaces;

        if (!options.includeSkip && (map.mtexinfos.at(face->texinfo).flags.extended & TEX_EXFLAG_SKIP))
            continue;

        /* emit a marksurface */
        do {
            map.bsp.dleaffaces.push_back(face->outputnumber.value());
            face = face->original; /* grab tjunction split faces */
        } while (face);
    }
    dleaf.nummarksurfaces = static_cast<int>(map.bsp.dleaffaces.size()) - dleaf.firstmarksurface;

    // FIXME-Q2: fill in other things
    dleaf.area = 1;
    dleaf.cluster = node->viscluster;
    dleaf.firstleafbrush = node->firstleafbrush;
    dleaf.numleafbrushes = node->numleafbrushes;
}

/*
==================
ExportDrawNodes
==================
*/
static void ExportDrawNodes(mapentity_t *entity, node_t *node)
{
    const size_t ourNodeIndex = map.bsp.dnodes.size();
    bsp2_dnode_t *dnode = &map.bsp.dnodes.emplace_back();

    // VectorCopy doesn't work since dest are shorts
    for (int32_t i = 0; i < 3; ++i) {
        dnode->mins[i] = floor(node->bounds.mins()[i]);
        dnode->maxs[i] = ceil(node->bounds.maxs()[i]);
    }

    dnode->planenum = ExportMapPlane(node->planenum);
    dnode->firstface = node->firstface;
    dnode->numfaces = node->numfaces;

    // recursively output the other nodes
    for (size_t i = 0; i < 2; i++) {
        if (node->children[i]->planenum == PLANENUM_LEAF) {
            // In Q2, all leaves must have their own ID even if they share solidity.
            if (options.target_game->id != GAME_QUAKE_II && node->children[i]->contents.is_solid(options.target_game)) {
                dnode->children[i] = PLANENUM_LEAF;
            } else {
                int32_t nextLeafIndex = static_cast<int32_t>(map.bsp.dleafs.size());
                const int32_t childnum = -(nextLeafIndex + 1);
                dnode->children[i] = childnum;
                ExportLeaf(entity, node->children[i]);
            }
        } else {
            const int32_t childnum = static_cast<int32_t>(map.bsp.dnodes.size());
            dnode->children[i] = childnum;
            ExportDrawNodes(entity, node->children[i]);

            // Important: our dnode pointer may be invalid after the recursive call, if the vector got resized.
            // So re-set the pointer.
            dnode = &map.bsp.dnodes[ourNodeIndex];
        }
    }

    // DarkPlaces asserts that the leaf numbers are different
    // if mod_bsp_portalize is 1 (default)
    // The most likely way it could fail is if both sides are the
    // shared CONTENTS_SOLID leaf (-1)
    Q_assert(!(dnode->children[0] == -1 && dnode->children[1] == -1));
    Q_assert(dnode->children[0] != dnode->children[1]);
}

/*
==================
ExportDrawNodes
==================
*/
void ExportDrawNodes(mapentity_t *entity, node_t *headnode, int firstface)
{
    // populate model struct (which was emitted previously)
    dmodelh2_t &dmodel = map.bsp.dmodels.at(entity->outputmodelnumber.value());
    dmodel.headnode[0] = static_cast<int32_t>(map.bsp.dnodes.size());
    dmodel.firstface = firstface;
    dmodel.numfaces = static_cast<int32_t>(map.bsp.dfaces.size()) - firstface;

    const size_t mapleafsAtStart = map.bsp.dleafs.size();

    if (headnode->planenum == PLANENUM_LEAF)
        ExportLeaf(entity, headnode);
    else
        ExportDrawNodes(entity, headnode);

    // count how many leafs were exported by the above calls
    dmodel.visleafs = static_cast<int32_t>(map.bsp.dleafs.size() - mapleafsAtStart);

    /* remove the headnode padding */
    for (size_t i = 0; i < 3; i++) {
        dmodel.mins[i] = headnode->bounds.mins()[i] + SIDESPACE + 1;
        dmodel.maxs[i] = headnode->bounds.maxs()[i] - SIDESPACE - 1;
    }
}

//=============================================================================

/*
==================
BeginBSPFile
==================
*/
void BeginBSPFile(void)
{
    // First edge must remain unused because 0 can't be negated
    map.bsp.dedges.emplace_back();
    Q_assert(map.bsp.dedges.size() == 1);

    // Leave room for leaf 0 (must be solid)
    auto &solid_leaf = map.bsp.dleafs.emplace_back();
    solid_leaf.contents = options.target_game->create_solid_contents().native;
    Q_assert(map.bsp.dleafs.size() == 1);
}

/*
 * Writes extended texinfo flags to a file so they can be read by the light tool.
 * Used for phong shading and other lighting settings on func_detail.
 */
static void WriteExtendedTexinfoFlags(void)
{
    bool needwrite = false;
    const int num_texinfo = map.numtexinfo();

    for (int i = 0; i < num_texinfo; i++) {
        if (map.mtexinfos.at(i).flags.needs_write()) {
            // this texinfo uses some extended flags, write them to a file
            needwrite = true;
            break;
        }
    }

    if (!needwrite)
        return;

    // sort by output texinfo number
    std::vector<mtexinfo_t> texinfos_sorted(map.mtexinfos);
    std::sort(texinfos_sorted.begin(), texinfos_sorted.end(),
        [](const mtexinfo_t &a, const mtexinfo_t &b) { return a.outputnum < b.outputnum; });

    options.szBSPName.replace_extension("texinfo");

    qfile_t texinfofile = SafeOpenWrite(options.szBSPName);

    if (!texinfofile)
        FError("Failed to open {}: {}", options.szBSPName, strerror(errno));

    extended_flags_header_t header;
    header.num_texinfo = map.bsp.texinfo.size();
    header.surfflags_size = sizeof(surfflags_t);

    SafeWrite(texinfofile, &header, sizeof(header));

    size_t count = 0;
    for (const auto &tx : texinfos_sorted) {
        if (!tx.outputnum.has_value())
            continue;

        Q_assert(count == tx.outputnum.value()); // check we are outputting them in the proper sequence

        SafeWrite(texinfofile, &tx.flags, sizeof(tx.flags));
        count++;
    }
    Q_assert(count == map.bsp.texinfo.size());
}

template<class C>
static void CopyVector(const std::vector<C> &vec, int *elementCountOut, C **arrayCopyOut)
{
    C *data = new C[vec.size()];
    memcpy(data, vec.data(), sizeof(C) * vec.size());

    *elementCountOut = vec.size();
    *arrayCopyOut = (C *)data;
}

static void CopyString(const std::string &string, bool addNullTermination, int *elementCountOut, void **arrayCopyOut)
{
    const size_t numBytes = addNullTermination ? string.size() + 1 : string.size();
    void *data = new uint8_t[numBytes];
    memcpy(data, string.data(), numBytes); // std::string::data() has null termination, so it's safe to copy it

    *elementCountOut = numBytes;
    *arrayCopyOut = data;
}

/*
=============
WriteBSPFile
=============
*/
static void WriteBSPFile()
{
    bspdata_t bspdata{};
    bspdata.bsp = std::move(map.bsp);

    bspdata.version = &bspver_generic;

    if (map.needslmshifts) {
        bspdata.bspx.copy("LMSHIFT", map.exported_lmshifts.data(), map.exported_lmshifts.size());
    }
    if (!map.exported_bspxbrushes.empty()) {
        bspdata.bspx.copy("BRUSHLIST", map.exported_bspxbrushes.data(), map.exported_bspxbrushes.size());
    }

    if (!ConvertBSPFormat(&bspdata, options.target_version)) {
        const bspversion_t *extendedLimitsFormat = options.target_version->extended_limits;

        if (!extendedLimitsFormat) {
            FError("No extended limits version of {} available", options.target_version->name);
        }

        LogPrint("NOTE: limits exceeded for {} - switching to {}\n", options.target_version->name,
            extendedLimitsFormat->name);

        Q_assert(ConvertBSPFormat(&bspdata, extendedLimitsFormat));
    }

    options.szBSPName.replace_extension("bsp");

    WriteBSPFile(options.szBSPName, &bspdata);
    LogPrint("Wrote {}\n", options.szBSPName);

    PrintBSPFileSizes(&bspdata);
}

/*
==================
FinishBSPFile
==================
*/
void FinishBSPFile(void)
{
    options.fVerbose = true;
    LogPrint(LOG_PROGRESS, "---- {} ----\n", __func__);

    WriteExtendedTexinfoFlags();
    WriteBSPFile();

    options.fVerbose = options.fAllverbose;
}

/*
==================
UpdateBSPFileEntitiesLump
==================
*/
void UpdateBSPFileEntitiesLump()
{
    bspdata_t bspdata;

    options.szBSPName.replace_extension("bsp");

    // load the .bsp
    LoadBSPFile(options.szBSPName, &bspdata);
    ConvertBSPFormat(&bspdata, &bspver_generic);

    mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);

    // replace the existing entities lump with map's exported entities
    bsp.dentdata = std::move(map.bsp.dentdata);

    // write the .bsp back to disk
    ConvertBSPFormat(&bspdata, bspdata.loadversion);
    WriteBSPFile(options.szBSPName, &bspdata);

    LogPrint("Wrote {}\n", options.szBSPName);
}
