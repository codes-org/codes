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
// https://github.com/LLNL/damselfly
// Please also read the LICENSE file for our notice and the LGPL.
//////////////////////////////////////////////////////////////////////////////

#include "stdio.h"
#include "stdlib.h"

//Usage ./binary num_groups num_rows num_columns intra_file inter_file

int main(int argc, char **argv) {
  int g = atoi(argv[1]);
  int r = atoi(argv[2]);
  int c = 1;

  FILE *intra = fopen(argv[3], "wb");
  FILE *inter = fopen(argv[4], "wb");
 
  int router = 0;
  int green = 0;
  for(int groups = 0; groups < g; groups++) {
    for(int rows = 0; rows < r; rows++) {
      if(groups == 0) {
        for(int rows1 = 0; rows1 < r; rows1++) {
          if(rows1 != rows) {
            int dest = rows1;
            fwrite(&router, sizeof(int), 1, intra);
            fwrite(&dest, sizeof(int), 1, intra);
            fwrite(&green, sizeof(int), 1, intra);
            //printf("INTRA %d %d %d\n", router, dest, green);
          }
        }
      }
      int myOff = router % (r * c);
      int numLink = g / (r*c);
      if(g % (r*c) != 0) {
        if((router % (r*c)) < (g % (r*c))) {
          numLink++;
        }
      }
      int myG = router / (r * c);
      for(int blues = 0; blues < numLink; blues++) {
        int dest = (blues * r * c) + myOff;
        if(dest != myG) {
          dest = (dest * r * c ) + (myG % (r * c));
          fwrite(&router, sizeof(int), 1, inter);
          fwrite(&dest, sizeof(int), 1, inter);
          printf("INTER %d %d\n", router, dest);
        }
      }
      router++;
    }
  }

  fclose(intra);
  fclose(inter);
}
