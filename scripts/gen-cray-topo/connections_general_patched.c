//////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2014, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory.
//
// Written by:
//     Nikhil Jain <nikhil.jain@acm.org>
//     Abhinav Bhatele <bhatele@llnl.gov>
//     Peer-Timo Bremer <ptbremer@llnl.gov>
//
// LLNL-CODE-678961. All rights reserved.
//
// This file is part of Damselfly. For details, see:
// https://github.com/scalability-llnl/damselfly
// Please also read the LICENSE file for our notice and the LGPL.
//////////////////////////////////////////////////////////////////////////////

#include "stdio.h"
#include "stdlib.h"

//Usage ./binary num_groups num_rows num_columns intra_file inter_file

int main(int argc, char **argv) {
  if(argc < 3) {
    printf("Correct usage: %s <num_group> <num_rows> <num_cols> <cons_across_groups> <cons_in_row> <cons_in_col> <intra_file> <inter_file>", argv[0]);
    exit(0);
  }

  int g = atoi(argv[1]);
  int r = atoi(argv[2]);
  int c = atoi(argv[3]);
  int g_p = atoi(argv[4]);
  int r_p = atoi(argv[5]);
  int c_p = atoi(argv[6]);

  int total_routers = g * r * c; 
  int routers_per_g = r * c; 

  FILE *intra = fopen(argv[7], "wb");
  FILE *inter = fopen(argv[8], "wb");

  int router = 0;
  int green = 0, black = 1;
  int groups = 0;
  for(int rows = 0; rows < r; rows++) {
    for(int cols = 0; cols < c; cols++) {
      for(int cols1 = 0; cols1 < c; cols1++) {
        if(cols1 != cols) {
          int dest = (rows * c) + cols1;
          for(int link = 0; link < c_p; link++) {
            fwrite(&router, sizeof(int), 1, intra);
            fwrite(&dest, sizeof(int), 1, intra);
            fwrite(&green, sizeof(int), 1, intra);
            printf("INTRA %d %d %d\n", router, dest, green);
          }
        }
      }
      for(int rows1 = 0; rows1 < r; rows1++) {
        if(rows1 != rows) {
          int dest = (rows1 * c) + cols;
          for(int link = 0; link < r_p; link++) {
            fwrite(&router, sizeof(int), 1, intra);
            fwrite(&dest, sizeof(int), 1, intra);
            fwrite(&black, sizeof(int), 1, intra);
            printf("INTRA %d %d %d\n", router, dest, black);
          }
        }
      }
      router++;
    }
  }

  for(int srcg = 0; srcg < g; srcg++) {
    for(int dstg = 0; dstg < g; dstg++) {
      if(srcg != dstg) {
        int nsrcg = srcg;
        int ndstg = dstg;
        if(srcg > dstg) {
          nsrcg--;
        } else {
          ndstg--;
        }
        int startSrc = ndstg * g_p;
        int startDst = nsrcg * g_p;
        for(int link = 0; link < g_p; link++) {
          int srcrB = srcg * routers_per_g, srcr;
          int dstrB = dstg * routers_per_g, dstr;
          srcr = srcrB + (startSrc + link) % routers_per_g;
          dstr = dstrB + (startDst + link) % routers_per_g;

          if(srcr >= total_routers || dstr >= total_routers)
            printf("\n connection between invalid routers src %d and dest %d ", srcr, dstr);

          fwrite(&srcr, sizeof(int), 1, inter);
          fwrite(&dstr, sizeof(int), 1, inter);
          printf("INTER %d %d srcg %d destg %d\n", srcr, dstr, srcg, dstg);
        }
      }
    }
  }

  fclose(intra);
  fclose(inter);
}
