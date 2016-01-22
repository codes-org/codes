/* usage mpirun -np n ./read_file_io 
n is the number of input bgp-log files */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <mpi.h>

struct dfly_samples
{
   uint64_t terminal_id;
   long fin_chunks_sample;
   long data_size_sample;
   double fin_hops_sample;
   double fin_chunks_time;
   double busy_time_sample;
   double end_time;
};

struct dfly_rtr_sample
{
    uint64_t router_id;
    double * busy_time;
    int64_t * link_traffic;
    double end_time;
};

static struct dfly_samples * event_array = NULL;
static struct dfly_rtr_sample * r_event_array = NULL;

int main( int argc, char** argv )
{
   int my_rank;
   int size;
   int i = 0, j = 0;
   int radix = atoi(argv[1]);
   if(!radix)
   {
        printf("\n Router radix should be specified ");
        MPI_Finalize();
        return -1;
   }

   printf("\n Router radix %d ", radix );
   MPI_Init(&argc, &argv);
   MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
   MPI_Comm_size(MPI_COMM_WORLD, &size);

   FILE* pFile;
   FILE* writeFile;

   char buffer_read[64];
   char buffer_write[64];

   sprintf(buffer_read, "dragonfly-cn-sampling-%d.bin", my_rank);
   pFile = fopen(buffer_read, "r+");

   struct stat st;
   stat(buffer_read, &st);
   long in_sz = st.st_size;
   event_array = malloc(in_sz);

   sprintf(buffer_write, "dragonfly-write-log.%d", my_rank);
   writeFile = fopen(buffer_write, "w+");

   if(pFile == NULL || writeFile == NULL)
   {
	fputs("\n File error ", stderr);
	return -1;
   }
   fseek(pFile, 0L, SEEK_SET);
   fread(event_array, sizeof(struct dfly_samples), in_sz / sizeof(struct dfly_samples), pFile);
   fprintf(writeFile, " Rank ID \t Finished chunks \t Data size \t Finished hops \t Time spent \t Busy time \t  Sample end time");
   for(i = 0; i < in_sz / sizeof(struct dfly_samples); i++)
   {
    fprintf(writeFile, "\n %ld \t %ld \t %ld \t %lf \t %lf \t %lf \t %lf ", event_array[i].terminal_id,
                                                               event_array[i].fin_chunks_sample,
                                                               event_array[i].data_size_sample,
                                                               event_array[i].fin_hops_sample, 
                                                               event_array[i].fin_chunks_time, 
                                                               event_array[i].busy_time_sample, 
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

    int sample_size = sizeof(struct dfly_rtr_sample) + radix * (sizeof(int64_t) + sizeof(double));
    for(i = 0; i < in_sz_rt / sample_size; i++)
    {
        r_event_array[i].busy_time = malloc(sizeof(double) * radix);
        r_event_array[i].link_traffic = malloc(sizeof(int64_t) * radix);
    }
    sprintf(buffer_rtr_write, "dragonfly-rtr-write-log.%d", my_rank);
    writeFile = fopen(buffer_rtr_write, "w+");

    if(writeFile == NULL || pFile == NULL)
    {
        fputs("\n File error ", stderr);
        MPI_Finalize();
        return -1;
    }
    fseek(pFile, 0L, SEEK_SET);
    fread(r_event_array, sample_size, in_sz_rt / sample_size, pFile); 
    fprintf(writeFile, "\n Router ID \t Busy time per channel \t Link traffic per channel \t Sample end time ");
    for(i = 0; i < in_sz_rt / sample_size; i++)
    {
        printf("\n %ld ", r_event_array[i].router_id);
        fprintf(writeFile, "\n %ld ", r_event_array[i].router_id);
        
        for(j = 0; j < radix; j++ )
        {
            printf("\n %lf ", r_event_array[i].busy_time[j]);
            fprintf(writeFile, " %lf ", r_event_array[i].busy_time[j]);
        }
        
        for(j = 0; j < radix; j++ )
            fprintf(writeFile, " %ld ", r_event_array[i].link_traffic[j]);
        
        fprintf(writeFile, "\n %lf ", r_event_array[i].end_time);
    }
    fclose(pFile);
    fclose(writeFile);
    MPI_Finalize();
}
