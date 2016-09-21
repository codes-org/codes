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
  int c = atoi(argv[3]);

  FILE *intra = fopen(argv[4], "w+");
  FILE *inter = fopen(argv[5], "w+");
 
  int router = 0;
  int dest = 0;
  int num_globs = 4;

  int green = 0;
  int black = 1;
  int blue = 2;

  printf("\n Num groups %d num_global_chans %d num_rows %d num_cols %d ", g, num_globs, r, c);

  for(int groups = 0; groups < g; groups++)
  {
      /* First connect the router to other routers in the same row */
      for(int rows = 0; rows < r; rows++) {
        int offset = c * rows;
        for(int out_col = 0; out_col < c; out_col++)
        {
            if(groups == 0)
            {
                /* Do it for group 0 only */
                for(int cols = 0; cols < c; cols++) {
                    dest = offset + cols;
                    
                    if((router % (c * r)) != dest)
                    {
                       fwrite(&router, sizeof(int), 1, intra);
                       fwrite(&dest, sizeof(int), 1, intra);
                       fwrite(&green, sizeof(int), 1, intra);
                       printf("\n INTRA Same row %d %d ", router, dest);
                    }
               }
                for(int r_up = 0; r_up < r; r_up++)
                {
                    dest = (c * r_up) + (router % c);
                    if((router % (c * r)) != dest)
                    {
                       fwrite(&router, sizeof(int), 1, intra);
                       fwrite(&dest, sizeof(int), 1, intra);
                       fwrite(&black, sizeof(int), 1, intra);
                       printf("\n INTRA Same col %d %d ", router, dest);
                    }
                }
           } // end if 
     
       // Now setup global connections
       //
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
          for(int pair = 0; pair < 2; pair++)
          {
            fwrite(&router, sizeof(int), 1, inter);
            fwrite(&dest, sizeof(int), 1, inter);
            printf("INTER %d %d %d \n", router, dest, blue);
          }
        }
      }
        router++;
    }
  }
  }
  fclose(intra);
  fclose(inter);
}
