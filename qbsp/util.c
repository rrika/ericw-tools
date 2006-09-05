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
// util.c

#include <conio.h>
#include <stdarg.h>
#include "qbsp.h"

extern char *rgszWarnings[cWarnings];
extern char *rgszErrors[cErrors];

int rgMemTotal[GLOBAL + 1];
int rgMemActive[GLOBAL + 1];
int rgMemPeak[GLOBAL + 1];

/*
==========
AllocMem
==========
*/
void *
AllocMem(int Type, int cElements, bool fZero)
{
    void *pTemp;
    int cSize;

    if (Type < 0 || Type > OTHER)
	Message(msgError, errInvalidMemType, Type);

    // For windings, cElements == number of points on winding
    if (Type == WINDING) {
	if (cElements > MAX_POINTS_ON_WINDING)
	    Message(msgError, errTooManyPoints, cElements);

	cSize = (int)((winding_t *)0)->points[cElements];

	// Set cElements to 1 so bookkeeping works OK
	cElements = 1;
    } else
	cSize = cElements * rgcMemSize[Type];

  Retry:
    pTemp = (void *)new char[cSize];

    if (pTemp == NULL) {
	char ch;

	printf
	    ("\bOut of memory.  Please close some apps and hit 'y' to try again,\n");
	printf("or any other key to exit -> ");
	ch = toupper(_getche());
	printf("\n");
	fflush(stdin);
	if (ch == 'Y')
	    goto Retry;
	Message(msgError, errOutOfMemory);
    }

    if (fZero)
	memset(pTemp, 0, cSize);

    // Special stuff for face_t
    if (Type == FACE)
	((face_t *)pTemp)->planenum = -1;

    rgMemTotal[Type] += cElements;
    rgMemActive[Type] += cElements;
    if (rgMemActive[Type] > rgMemPeak[Type])
	rgMemPeak[Type] = rgMemActive[Type];

    // Also keep global statistics
    rgMemTotal[GLOBAL] += cSize;
    rgMemActive[GLOBAL] += cSize;
    if (rgMemActive[GLOBAL] > rgMemPeak[GLOBAL])
	rgMemPeak[GLOBAL] = rgMemActive[GLOBAL];

    return pTemp;
}


/*
==========
FreeMem
==========
*/
void
FreeMem(void *pMem, int Type, int cElements)
{
    rgMemActive[Type] -= cElements;
    rgMemActive[GLOBAL] -= cElements * rgcMemSize[Type];

    delete pMem;
}


/*
==========
PrintMem
==========
*/
void
PrintMem(void)
{
    char *rgszMemTypes[] =
	{ "BSPEntity", "BSPPlane", "BSPTex", "BSPVertex", "BSPVis",
	"BSPNode", "BSPTexinfo", "BSPFace", "BSPLight", "BSPClipnode",
	    "BSPLeaf",
	"BSPMarksurface", "BSPEdge", "BSPSurfedge", "BSPModel", "Mapface",
	    "Mapbrush",
	"Mapentity", "Winding", "Face", "Plane", "Portal", "Surface", "Node",
	    "Brush",
	"Miptex", "World verts", "World edges", "Hash verts", "Other (bytes)",
	"Total (bytes)"
    };
    int i;

    if (options.fVerbose) {
	Message(msgLiteral,
		"\nData type         Current     Peak     Total   Peak Bytes\n");
	for (i = 0; i <= GLOBAL; i++)
	    Message(msgLiteral, "%-16s %8d %8d %10d %9d\n", rgszMemTypes[i],
		    rgMemActive[i], rgMemPeak[i], rgMemTotal[i],
		    rgMemPeak[i] * rgcMemSize[i]);
    } else
	Message(msgLiteral, "Bytes used: %d\n", rgMemPeak[GLOBAL]);
}

/*
============
FreeAllMem
============
*/
void
FreeAllMem(void)
{
    int i, j;
    epair_t *ep, *next;

    for (i = 0; i < map.cEntities; i++) {
	for (ep = map.rgEntities[i].epairs; ep; ep = next) {
	    next = ep->next;
	    if (ep->key)
		FreeMem(ep->key, OTHER, strlen(ep->key) + 1);
	    if (ep->value)
		FreeMem(ep->value, OTHER, strlen(ep->value) + 1);
	    FreeMem(ep, OTHER, sizeof(epair_t));
	}
	for (j = 0; j < BSP_LUMPS; j++)
	    if (map.rgEntities[i].pData[j])
		FreeMem(map.rgEntities[i].pData[j], j,
			map.rgEntities[i].cData[j]);
    }

    FreeMem(validfaces, OTHER, sizeof(face_t *) * cPlanes);
    FreeMem(pPlanes, PLANE, cPlanes);
    FreeMem(map.rgFaces, MAPFACE, map.cFaces);
    FreeMem(map.rgBrushes, MAPBRUSH, map.cBrushes);
    FreeMem(map.rgEntities, MAPENTITY, map.cEntities);

}


/*
=================
Message

Generic output of errors, warnings, stats, etc
=================
*/
void
Message(int msgType, ...)
{
    va_list argptr;
    char szBuffer[512];
    char *szFmt;
    int ErrType;
    int Cur, Total;
    static bool fInPercent = false;

    va_start(argptr, msgType);

    // Exit if necessary
    if ((msgType == msgStat || msgType == msgProgress)
	&& (!options.fVerbose || options.fNoverbose))
	return;
    else if (msgType == msgPercent
	     && (options.fNopercent || options.fNoverbose))
	return;

    // Grab proper args
    if (msgType == msgWarning || msgType == msgError)
	ErrType = va_arg(argptr, int);

    else if (msgType == msgPercent) {
	Cur = va_arg(argptr, int);
	Total = va_arg(argptr, int);
    } else
	szFmt = va_arg(argptr, char *);

    // Handle warning/error-in-percent properly
    if (fInPercent && msgType != msgPercent) {
	printf("\r");
	fInPercent = false;
    }

    switch (msgType) {
    case msgWarning:
	if (ErrType >= cWarnings)
	    printf("Internal error: unknown ErrType in Message!\n");
	sprintf(szBuffer, "*** WARNING %02d: ", ErrType);
	vsprintf(szBuffer + strlen(szBuffer), rgszWarnings[ErrType], argptr);
	strcat(szBuffer, "\n");
	break;

    case msgError:
	if (ErrType >= cErrors)
	    printf("Program error: unknown ErrType in Message!\n");
	sprintf(szBuffer, "*** ERROR %02d: ", ErrType);
	vsprintf(szBuffer + strlen(szBuffer), rgszErrors[ErrType], argptr);
	puts(szBuffer);
	LogFile.Printf("%s\n", szBuffer);
	LogFile.Close();
	exit(1);
	break;

    case msgLiteral:
	// Output as-is to screen and log file
	vsprintf(szBuffer, szFmt, argptr);
	break;

    case msgStat:
	// Output as-is to screen and log file
	strcpy(szBuffer, "\t");
	vsprintf(szBuffer + strlen(szBuffer), szFmt, argptr);	// Concatenate
	strcat(szBuffer, "\n");
	break;

    case msgProgress:
	// Output as-is to screen and log file
	strcpy(szBuffer, "---- ");
	vsprintf(szBuffer + strlen(szBuffer), szFmt, argptr);	// Concatenate
	strcat(szBuffer, " ----\n");
	break;

    case msgPercent:
	// Calculate the percent complete.  Only output if it changes.
	if (((Cur + 1) * 100) / Total == (Cur * 100) / Total)
	    return;

	sprintf(szBuffer, "\r%3d%%", ((Cur + 1) * 100) / Total);

	// Handle output formatting properly
	fInPercent = true;
	msgType = msgScreen;
	break;

    case msgFile:
	// Output only to the file
	vsprintf(szBuffer, szFmt, argptr);
	break;

    case msgScreen:
	// Output only to the screen
	vsprintf(szBuffer, szFmt, argptr);
	break;

    default:
	printf("Unhandled msgType in message!\n");
	return;
    }

    if (msgType != msgFile)
	printf("%s", szBuffer);
    if (msgType != msgScreen)
	LogFile.Printf("%s", szBuffer);

    va_end(argptr);
}
