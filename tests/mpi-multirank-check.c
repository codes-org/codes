/*
 * MPI multi-rank launch guard.
 *
 * Asserts that mpiexec actually forms a multi-rank MPI_COMM_WORLD. A broken MPI
 * launcher (notably Ubuntu 24.04's mpich 4.2.0-5build3, whose Hydra starts every
 * rank as its own singleton) reports MPI_Comm_size == 1 even under `mpiexec -np
 * N`. That silently degrades CODES's optimistic (--sync=3) model tests to N
 * independent *sequential* runs -- ROSS prints "Defaulting to Sequential
 * Simulation, not enough PEs defined" per rank -- while the tests still exit 0
 * and pass. This guard turns that silent degradation into a hard ctest failure,
 * now and for any future environment with the same problem.
 *
 * Exits 0 iff the world size equals the expected size (argv[1], default 2).
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank = 0, size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    int expected = (argc > 1) ? atoi(argv[1]) : 2;

    if (rank == 0) {
        printf("MPI multi-rank guard: MPI_COMM_WORLD size = %d (expected %d)\n", size, expected);
        if (size != expected) {
            fprintf(stderr,
                    "ERROR: mpiexec did not form a %d-rank world (got %d). The MPI "
                    "launcher is starting singletons, so parallel/optimistic "
                    "(--sync=3) tests would silently run sequential. See the Ubuntu "
                    "24.04 mpich 4.2.0 PMI bug.\n",
                    expected, size);
        }
    }

    MPI_Finalize();
    return (size == expected) ? 0 : 1;
}
