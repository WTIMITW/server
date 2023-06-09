/* -------------------------------------------------------------------------
 *
 * nodeSort.h
 *
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeSort.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef NODESORT_H
#define NODESORT_H

#include "nodes/execnodes.h"

extern SortState* ExecInitSort(Sort* node, EState* estate, int eflags);
extern void ExecEndSort(SortState* node);
extern void ExecSortMarkPos(SortState* node);
extern void ExecSortRestrPos(SortState* node);
extern void ExecReScanSort(SortState* node);
extern long ExecGetMemCostSort(Sort* node);
extern void ExecEarlyFreeSort(SortState* node);
extern void ExecReSetSort(SortState* node);

#endif /* NODESORT_H */
