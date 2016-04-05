/* usage mpirun -np n ./read_file_io 
n is the number of input bgp-log files */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <mpi.h>
#define RADIX 16

struct dfly_samples
{
   uint64_t terminal_id;
   long fin_chunks_sample;
   long data_size_sample;
   double fin_hops_sample;
   double fin_chunks_time;
   double busy_time_sample;
   double end_time;
   long fwd_events;
   long rev_events;
};

struct dfly_rtr_sample
{
    uint64_t router_id;
    double busy_time[RADIX];
    int64_t link_traffic[RADIX];
    double end_time;
    long fwd_events;
    long rev_events;
};

static struct dfly_samples * event_array = NULL;
static struct dfly_rtr_sample * r_event_array = NULL;

int main( int argc, char** argv )
{
   int my_rank;
   int size;
   int i = 0, j = 0;
   /*int RADIX = atoi(argv[1]);
   if(!RADIX)
   {
        printf("\n Router RADIX should be specified ");
        MPI_Finalize();
        return -1;
   }*/

   MPI_Init(&argc, &argv);
   MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
   MPI_Comm_size(MPI_COMM_WORLD, &size);

   FILE* pFile;
   FILE* writeFile;
   FILE* writeRouterFile;

   char buffer_read[64];
   char buffer_write[64];

   sprintf(buffer_read, "dragonfly-cn-sampling-%d.bin", my_rank);
   pFile = fopen(buffer_read, "r+");

   struct stat st;
   stat(buffer_read, &st);
   long in_sz = st.st_size;
   event_array = malloc(in_sz);

   sprintf(buffer_write, "dragonfly-write-log.%d", my_rank);
   writeFile = fopen(buffer_write, "w");

   if(pFile == NULL || writeFile == NULL)
   {
	fputs("\n File error ", stderr);
	return -1;
   }
   fseek(pFile, 0L, SEEK_SET);
   fread(event_array, sizeof(struct dfly_samples), in_sz / sizeof(struct dfly_samples), pFile);
   fprintf(writeFile, " Rank_ID Finished_chunks Data_size Finished_hops Time_spent busy_time fwd_events rev_events sample_end_time");
   for(i = 0; i < in_sz / sizeof(struct dfly_samples); i++)
   {
    printf("\n Terminal id %ld ", event_array[i].terminal_id);
    fprintf(writeFile, "\n %ld %ld %ld %lf %lf %lf %ld %ld %lf ", event_array[i].terminal_id,
                                                               event_array[i].fin_chunks_sample,
                                                               event_array[i].data_size_sample,
                                                               event_array[i].fin_hops_sample, 
                                                               event_array[i].fin_chunks_time, 
                                                               event_array[i].busy_time_sample, 
                                                               event_array[i].fwd_events,
                                                               event_array[i].rev_events,
                                                               event_array[i].end_time);
   }
    fclose(pFile);
    fclose(writeFile);

    printf("\n Now reading router file ");
    /* Now read the router sampling file */
    char buffer_rtr_read[64];
    char buffer_rtr_write[64];

    sprintf(buffer_rtr_read, "dragonfly-router-sampling-%d.bin", my_rank);
    pFile = fopen(buffer_rtr_read, "r+");

    struct stat st2;
    stat(buffer_rtr_read, &st2);
    long in_sz_rt = st2.st_size;

    r_event_array = malloc(in_sz_rt);

    int sample_size = sizeof(struct dfly_rtr_sample);
    sprintf(buffer_rtr_write, "dragonfly-rtr-write-log.%d", my_rank);
    writeRouterFile = fopen(buffer_rtr_write, "w");

    if(writeRouterFile == NULL || pFile == NULL)
    {
        fputs("\n File error ", stderr);
        MPI_Finalize();
        return -1;
    }
    fseek(pFile, 0L, SEEK_SET);
    fread(r_event_array, sample_size, in_sz_rt / sample_size, pFile); 
    fprintf(writeRouterFile, "\n Router_ID Busy_time_per_channel Link_traffic_per_channel Sample_end_time fwd_events reverse_events");
    //printf("\n Sample size %d in_sz_rt %ld ", in_sz_rt / sample_size, in_sz_rt);
    for(i = 0; i < in_sz_rt / sample_size; i++)
    {
        //printf("\n %ld ", r_event_array[i].router_id);
        fprintf(writeRouterFile, "\n %ld ", r_event_array[i].router_id);
        
        for(j = 0; j < RADIX; j++ )
        {
            //printf("\n %lf ", r_event_array[i].busy_time[j]);
            fprintf(writeRouterFile, " %lf ", r_event_array[i].busy_time[j]);
        }
        
        for(j = 0; j < RADIX; j++ )
        {
            //printf("\n link traffic %ld ", r_event_array[i].link_traffic[j]);
            fprintf(writeRouterFile, " %ld ", r_event_array[i].link_traffic[j]);
        }
        fprintf(writeRouterFile, " %lf \n", r_event_array[i].end_time);
        fprintf(writeRouterFile, " %ld ", r_event_array[i].fwd_events);
        fprintf(writeRouterFile, " %ld \n", r_event_array[i].rev_events);
    }
    fclose(pFile);
    fclose(writeRouterFile);
    MPI_Finalize();
}
