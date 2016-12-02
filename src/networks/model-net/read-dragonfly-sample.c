/* usage mpirun -np n ./read_file_io 
n is the number of input bgp-log files */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <mpi.h>
#include <assert.h>
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

struct mpi_workload_sample
{
    int nw_id;
    int app_id;
    unsigned long num_sends_sample;
    unsigned long num_bytes_sample;
    unsigned long num_waits_sample;
    double sample_end_time;
};
static struct dfly_samples * event_array = NULL;
static struct dfly_rtr_sample * r_event_array = NULL;
static struct mpi_workload_sample * mpi_event_array = NULL;

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

   sprintf(buffer_write, "dragonfly-write-log-%d.dat", my_rank);
   writeFile = fopen(buffer_write, "w");

   if(pFile == NULL || writeFile == NULL)
   {
	fputs("\n File error ", stderr);
	return -1;
   }
   if(my_rank == 0)
   {
        char meta_filename[128];
        sprintf(meta_filename, "dragonfly-write-log.meta");

        FILE * fp_meta = fopen(meta_filename, "w+");
        fprintf(fp_meta, "Rank_ID num_finished_chunks data_size_finished(bytes) finished_hops time_spent(ns) busy_time(ns) num_fwd_events num_rev_events sample_end_time(ns)");
        fclose(fp_meta);
   }
   fseek(pFile, 0L, SEEK_SET);
   fread(event_array, sizeof(struct dfly_samples), in_sz / sizeof(struct dfly_samples), pFile);
   for(i = 0; i < in_sz / sizeof(struct dfly_samples); i++)
   {
    printf("\n Terminal id %ld ", event_array[i].terminal_id);
    fprintf(writeFile, "%ld %ld %ld %lf %lf %lf %ld %ld %lf \n", event_array[i].terminal_id,
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
    sprintf(buffer_rtr_write, "dragonfly-rtr-write-%d.dat", my_rank);
    writeRouterFile = fopen(buffer_rtr_write, "w");

    if(writeRouterFile == NULL || pFile == NULL)
    {
        fputs("\n File error ", stderr);
        MPI_Finalize();
        return -1;
    }
    fseek(pFile, 0L, SEEK_SET);
   
    if(my_rank == 0)
    {
        char rtr_meta_filename[128];
        sprintf(rtr_meta_filename, "dragonfly-rtr-write-log.meta");

        FILE * fp_rtr_meta = fopen(rtr_meta_filename, "w+");
        fprintf(fp_rtr_meta, "%d entries for busy time and link traffic \n", RADIX);
        fprintf(fp_rtr_meta, "Format: Router_ID Busy_time_per_channel(ns) Link_traffic_per_channel(ns) Sample_end_time(ns) fwd_events reverse_events");
        fclose(fp_rtr_meta);
   }
    fread(r_event_array, sample_size, in_sz_rt / sample_size, pFile); 
    //printf("\n Sample size %d in_sz_rt %ld ", in_sz_rt / sample_size, in_sz_rt);
    for(i = 0; i < in_sz_rt / sample_size; i++)
    {
        //printf("\n %ld ", r_event_array[i].router_id);
        fprintf(writeRouterFile, "%ld ", r_event_array[i].router_id);
        
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
       fprintf(writeRouterFile, " %lf ", r_event_array[i].end_time);
        fprintf(writeRouterFile, " %ld ", r_event_array[i].fwd_events);
        fprintf(writeRouterFile, " %ld \n", r_event_array[i].rev_events);
    }
    fclose(pFile);
    fclose(writeRouterFile);
    
    sprintf(buffer_rtr_read, "mpi-aggregate-logs-%d.bin", my_rank);
    pFile = fopen(buffer_rtr_read, "r+");
    assert(pFile);

    struct stat st3;
    stat(buffer_rtr_read, &st3);
    long in_sz_mpi = st3.st_size;

    mpi_event_array = malloc(in_sz_mpi);
    int mpi_sample_sz = sizeof(struct mpi_workload_sample);
    sprintf(buffer_rtr_write, "dragonfly-mpi-write-logs-%d.dat", my_rank);
    writeFile = fopen(buffer_rtr_write, "w+");
    assert(writeFile);

    if(my_rank == 0)
    {
        char ops_meta_filename[128];
        sprintf(ops_meta_filename, "dragonfly-mpi-write-logs.meta");

        FILE * fp_ops_meta = fopen(ops_meta_filename, "w+");
        fprintf(fp_ops_meta, "network_node_id app_id num_sends num_bytes_sent num_waits sample_end_time(ns)");
        fclose(fp_ops_meta);
   }
    fread(mpi_event_array, mpi_sample_sz, in_sz_mpi / mpi_sample_sz, pFile);
    for(i = 0; i < in_sz_mpi / mpi_sample_sz; i++)
    {
        fprintf(writeFile, "\n %d %d %lu %lu %lu %lf ",
                mpi_event_array[i].nw_id,
                mpi_event_array[i].app_id,
                mpi_event_array[i].num_sends_sample,
                mpi_event_array[i].num_bytes_sample,
                mpi_event_array[i].num_waits_sample,
                mpi_event_array[i].sample_end_time);
    }
    fclose(pFile);
    fclose(writeFile);

    MPI_Finalize();
}
