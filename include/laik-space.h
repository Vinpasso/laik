/* 
 * This file is part of the LAIK parallel container library.
 * Copyright (c) 2017 Josef Weidendorfer
 * 
 * LAIK is free software: you can redistribute it and/or modify  
 * it under the terms of the GNU Lesser General Public License as   
 * published by the Free Software Foundation, version 3.
 *
 * LAIK is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _LAIK_SPACE_H_
#define _LAIK_SPACE_H_

#include "laik.h"

#include <stdint.h>
#include <stdbool.h>

/*********************************************************************/
/* LAIK Spaces - Distributed partitioning of index spaces
 *********************************************************************/


// generic partition types, may need parameters
typedef enum _Laik_PartitionType {
    LAIK_PT_None = 0,

    // base
    LAIK_PT_Master,   // only one task has access to all elements
    LAIK_PT_All,      // all tasks have access to all elements
    LAIK_PT_Stripe,   // continous distinct ranges, covering all elements

    // coupled
    LAIK_PT_Copy,     // copy borders from base partitioning
    LAIK_PT_Halo,     // extend a partitioning at borders
    LAIK_PT_Neighbor, // extend a partitioning with neighbor parts
} Laik_PartitionType;

// access permission to partitions
typedef enum _Laik_AccessPermission {
    LAIK_AP_None = 0,
    LAIK_AP_ReadOnly,
    LAIK_AP_WriteOnly, // promises complete overwriting
    LAIK_AP_ReadWrite,
    LAIK_AP_Plus,      // + reduction, multiple writers
    LAIK_AP_Times,     // * reduction, multiple writers
    LAIK_AP_Min,       // min reduction, multiple writers
    LAIK_AP_Max        // max reduction, multiple writers
} Laik_AccessPermission;

// a point in an index space
typedef struct _Laik_Index Laik_Index;
struct _Laik_Index {
    uint64_t i[3]; // at most 3 dimensions
};

// a rectangle-shaped slice from an index space [from;to[
typedef struct _Laik_Slice Laik_Slice;
struct _Laik_Slice {
  Laik_Index from, to;
};

// a participating task in the distribution of an index space
typedef struct _Laik_Task Laik_Task;

// an index space (regular and continous, up to 3 dimensions)
typedef struct _Laik_Space Laik_Space;

// a partitioning of an index space with same access permission
typedef struct _Laik_Partitioning Laik_Partitioning;

// set of partitionings to make consistent at the same time
typedef struct _Laik_PartGroup Laik_PartGroup;

// communication requirements when switching partitioning groups
typedef struct _Laik_Transition Laik_Transition;



/*********************************************************************/
/* LAIK API for distributed index spaces
 *********************************************************************/

// is this a reduction?
bool laik_is_reduction(Laik_AccessPermission p);
// includes this read access?
bool laik_is_read(Laik_AccessPermission);
// includes this write access?
bool laik_is_write(Laik_AccessPermission);

// create a new index space object (initially invalid)
Laik_Space* laik_new_space(Laik_Instance* i);

// create a new index space object with an initial size
Laik_Space* laik_new_space_1d(Laik_Instance* i, uint64_t s1);
Laik_Space* laik_new_space_2d(Laik_Instance* i,
                              uint64_t s1, uint64_t s2);
Laik_Space* laik_new_space_3d(Laik_Instance* i,
                              uint64_t s1, uint64_t s2, uint64_t s3);

// free a space with all resources depending on it (e.g. paritionings)
void laik_free_space(Laik_Space* s);

// set a space a name, for debug output
void laik_set_space_name(Laik_Space* s, char* n);

// change the size of an index space, eventually triggering a repartitiong
void laik_change_space_1d(Laik_Space* s, uint64_t s1);
void laik_change_space_2d(Laik_Space* s,
                          uint64_t s1, uint64_t s2);
void laik_change_space_3d(Laik_Space* s,
                          uint64_t s1, uint64_t s2, uint64_t s3);

// is the given slice empty?
bool laik_slice_isEmpty(int dims, Laik_Slice* slc);

// get the intersection of 2 slices; return 0 if intersection is empty
Laik_Slice* laik_slice_intersect(int dims, Laik_Slice* s1, Laik_Slice* s2);

// create a new partitioning on a space
Laik_Partitioning*
laik_new_base_partitioning(Laik_Space* space,
                           Laik_PartitionType pt,
                           Laik_AccessPermission ap);

// set index-wise and task-wise weight getter interface
void laik_set_weight_IF(Laik_Partitioning*,
                        int (*)(Laik_Partitioning*, Laik_Index*),
                        int (*)(Laik_Partitioning*, int));

// for multiple-dimensional spaces, set dimension to partition (default is 0)
void laik_set_partitioning_dimension(Laik_Partitioning* p, int d);

// create a new partitioning based on another one on the same space
Laik_Partitioning*
laik_new_coupled_partitioning(Laik_Partitioning* p,
                              Laik_PartitionType pt,
                              Laik_AccessPermission ap);

// create a new partitioning based on another one on a different space
// this also needs to know which dimensions should be coupled
Laik_Partitioning*
laik_new_spacecoupled_partitioning(Laik_Partitioning* p,
                                   Laik_Space* s, int from, int to,
                                   Laik_PartitionType pt,
                                   Laik_AccessPermission ap);

// free a partitioning with related resources
void laik_free_partitioning(Laik_Partitioning* p);

// give a partitioning a name, for debug output
void laik_set_partitioning_name(Laik_Partitioning* p, char* n);

// make sure partitioning borders are up to date
void laik_update_partitioning(Laik_Partitioning* p);

// append a partitioning to a partioning group whose consistency should
// be enforced at the same point in time
void laik_append_partitioning(Laik_PartGroup* g, Laik_Partitioning* p);

// Calculate communication required for transitioning between partitionings
Laik_Transition* laik_calc_transitionP(Laik_Partitioning* from,
                                       Laik_Partitioning* to);

// Calculate communication for transitioning between partitioning groups
Laik_Transition* laik_calc_transitionG(Laik_PartGroup* from,
                                       Laik_PartGroup* to);

// enforce consistency for the partitioning group, depending on previous
void laik_enforce_consistency(Laik_Instance* i, Laik_PartGroup* g);

// set a weight for each participating task in a partitioning, to be
//  used when a repartitioning is requested
void laik_set_partition_weights(Laik_Partitioning*p, int* w);

// change an existing base partitioning
void laik_repartition(Laik_Partitioning* p, Laik_PartitionType pt);

// couple different LAIK instances via spaces:
// one partition of calling task in outer space is mapped to inner space
void laik_couple_nested(Laik_Space* outer, Laik_Space* inner);


#endif // _LAIK_SPACE_H_
