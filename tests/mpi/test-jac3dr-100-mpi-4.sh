#!/bin/sh
LAIK_BACKEND=mpi ${MPIEXEC-mpiexec} -n 4 ../../examples/jac3d -r -s 100 > test-jac3dr-100-mpi-4.out
cmp test-jac3dr-100-mpi-4.out "$(dirname -- "${0}")/test-jac3d-100.expected"
