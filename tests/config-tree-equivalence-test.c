/*
 * config-tree-equivalence-test <config-a> <config-b>
 *
 * Loads two config files through the production loader (configuration_load, so
 * a .conf goes through the legacy text parser and a .yaml/.json through the YAML
 * front-end, including collective include resolution) and asserts the two
 * resulting config trees are equal with cf_equal.
 *
 * This is a cheap, per-key check: it never runs a model, so unlike the lp-io /
 * marker equivalence tests it compares *every* section, key and value between a
 * .conf and its YAML twin -- catching a defaulted or drifted key that a short
 * behavioral run can mask -- at a fraction of the cost. configuration_load uses
 * MPI (collective reads), so the binary initializes MPI and runs as a singleton;
 * the comparison itself is identical on every rank.
 *
 * Exit status: 0 if the trees are equal, 1 if they differ (cf_equal_report has
 * printed which section/key diverged), 2 on a usage or load error.
 */

#include <mpi.h>
#include <stdio.h>

#include <codes/configuration.h>
#include <codes/configfile.h>

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (argc != 3) {
        if (rank == 0)
            fprintf(stderr, "usage: %s <config-a> <config-b>\n", argv[0]);
        MPI_Finalize();
        return 2;
    }

    ConfigHandle ha = NULL;
    ConfigHandle hb = NULL;
    int rc = 0;

    if (configuration_load(argv[1], MPI_COMM_WORLD, &ha) != 0 || ha == NULL) {
        fprintf(stderr, "config-tree-equivalence: could not load \"%s\"\n", argv[1]);
        rc = 2;
        goto done;
    }
    if (configuration_load(argv[2], MPI_COMM_WORLD, &hb) != 0 || hb == NULL) {
        fprintf(stderr, "config-tree-equivalence: could not load \"%s\"\n", argv[2]);
        rc = 2;
        goto done;
    }

    if (cf_equal_report(ha, hb, stderr)) {
        if (rank == 0)
            printf("config trees identical:\n  A = %s\n  B = %s\n", argv[1], argv[2]);
        rc = 0;
    } else {
        fprintf(stderr, "config-tree-equivalence: trees differ:\n  A = %s\n  B = %s\n", argv[1],
                argv[2]);
        rc = 1;
    }

done:
    if (ha)
        cf_free(ha);
    if (hb)
        cf_free(hb);
    MPI_Finalize();
    return rc;
}
