#!/bin/sh
LAIK_BACKEND=mpi ${MPIEXEC-mpiexec} -n 4 ../../examples/jac3d -n -r -s 100 > test-jac3dnr-100-mpi-4.out
cmp test-jac3dnr-100-mpi-4.out "$(dirname -- "${0}")/test-jac3dn-100.expected"
