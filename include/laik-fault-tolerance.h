//
// Created by Vincent Bode on 11/05/2019.
//

#ifndef LAIK_LAIK_FAULT_TOLERANCE_H
#define LAIK_LAIK_FAULT_TOLERANCE_H

typedef enum {
    LAIK_FT_NODE_OK = 1,
    LAIK_FT_NODE_FAULT = -1
} LAIK_FT_NODE_STATUS;

struct _Laik_Checkpoint {
    Laik_Space* space;
    Laik_Data* data;
};

typedef struct _Laik_Checkpoint Laik_Checkpoint;

Laik_Checkpoint* laik_checkpoint_create(Laik_Instance *laikInstance, Laik_Space *space, Laik_Data *data,
                                       Laik_Partitioner *backupPartitioner, Laik_Group *backupGroup,
                                       enum _Laik_ReductionOperation reductionOperation);

void laik_checkpoint_restore(Laik_Instance *laikInstance, Laik_Checkpoint *checkpoint, Laik_Space *space, Laik_Data *data);

int laik_failure_check_nodes(Laik_Instance *laikInstance, Laik_Group *checkGroup, int (*failedNodes));
int laik_failure_eliminate_nodes(Laik_Instance *instance, int count, int (*nodeStatuses));

bool laik_checkpoint_remove_failed_slices(Laik_Checkpoint *checkpoint, int (*nodeStatuses)[]);

Laik_Group* laik_world_fault_tolerant(Laik_Instance* instance);

int laik_location_get_world_offset(Laik_Group *group, int id);

#endif //LAIK_LAIK_FAULT_TOLERANCE_H