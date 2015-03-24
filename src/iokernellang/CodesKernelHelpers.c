/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include "CodesKernelHelpers.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <libgen.h>

#define CK_LINE_LIMIT 8192
#define CL_DEFAULT_GID 0

char * code_kernel_helpers_kinstToStr(int inst)
{
    switch(inst)
    {
        case WRITEAT:
            return "WRITEAT";
	case READAT:
	    return "READAT";
        case GETGROUPRANK:
            return "GETGROUPRANK";
        case GETGROUPSIZE:
            return "GETGROUPSIZE";
        case CLOSE:
            return "CLOSE";
        case OPEN:
            return "OPEN";
        case SYNC:
            return "SYNC";
        case SLEEP:
            return "SLEEP";
        case EXIT:
            return "EXIT";
        case DELETE:
            return "DELETE";
        default:
            return "UNKNOWN";
    };
    
    return "UNKNOWN";
}

char * code_kernel_helpers_cleventToStr(int inst)
{
    switch(inst)
    {
        case CL_WRITEAT:
            return "CL_WRITEAT";
	case CL_READAT:
	    return "CL_READAT";
        case CL_GETRANK:
            return "CL_GETRANK";
        case CL_GETSIZE:
            return "CL_GETSIZE";
        case CL_CLOSE:
            return "CL_CLOSE";
        case CL_OPEN:
            return "CL_OPEN";
        case CL_SYNC:
            return "CL_SYNC";
        case CL_SLEEP:
            return "CL_SLEEP";
        case CL_EXIT:
            return "CL_EXIT";
        case CL_DELETE:
            return "CL_DELETE";
        default:
            return "CL_UNKNOWN";
    };
    
    return "CL_UNKNOWN";
}

static int convertKLInstToEvent(int inst)
{
    switch(inst)
    {
        case WRITEAT:
            return CL_WRITEAT;
	case READAT:
	    return CL_READAT;
        case GETGROUPRANK:
            return CL_GETRANK;
        case GETGROUPSIZE:
            return CL_GETSIZE;
        case CLOSE:
            return CL_CLOSE;
        case OPEN:
            return CL_OPEN;
        case SYNC:
            return CL_SYNC;
        case SLEEP:
            return CL_SLEEP;
        case EXIT:
            return CL_EXIT;
        case DELETE:
            return CL_DELETE;
        default:
            return CL_UNKNOWN;
    };

    return CL_UNKNOWN;
}

static void codes_kernel_helper_parse_cf(char * io_kernel_path,
        char * io_kernel_meta_path, int task_rank, int max_ranks_default,
        codes_workload_info * task_info, int use_relpath)
{
       int foundit = 0;
       char line[CK_LINE_LIMIT];
       FILE * ikmp = NULL;

       /* open the config file */
       ikmp = fopen(io_kernel_meta_path, "r");
       if(ikmp == NULL)
       {
           fprintf(stderr, "%s:%i could not open meta file (%s)... bail\n", __func__,
                   __LINE__, io_kernel_meta_path);
           exit(1);
       }

       /* for each line in the config file */
       while(fgets(line, CK_LINE_LIMIT, ikmp) != NULL)
       {
               char * token = NULL;
               int min = 0;
               int max = 0;
               int gid = 0;
               char * ctx = NULL;

               /* parse the first element... the gid */
               token = strtok_r(line, " \n", &ctx);
               if(token)
                   gid = atoi(token);

               if(gid == CL_DEFAULT_GID)
               {
                   fprintf(stderr, "%s:%i incorrect GID detected in kernel meta\
                           file. Cannot use the reserved GID\
                           CL_DEFAULT_GID(%i)\n", __func__, __LINE__,
                           CL_DEFAULT_GID);
               }

               /* parse the second element... min rank */
               token = strtok_r(NULL, " \n", &ctx);
               if(token)
                       min = atoi(token);

               /* parse the third element... max rank */
               token = strtok_r(NULL, " \n", &ctx);
               if(token)
                       max = atoi(token);

               /* parse the last element... kernel path */
               token = strtok_r(NULL, " \n", &ctx);
               if(token) {
                       if (use_relpath){
                           /* posix dirname overwrites argument :(, need to
                            * prevent that */
                           char *tmp_path = strdup(io_kernel_meta_path);
                           sprintf(io_kernel_path, "%s/%s", dirname(tmp_path), token);
                           free(tmp_path);
                       }
                       else{
                           strcpy(io_kernel_path, token);
                       }
               }

               /* if our rank is on this range... end processing of the config
                * file */
               if(task_rank >= min && (max == -1 || task_rank <= max))
               {
                       task_info->group_id = gid;
                       task_info->min_rank = min;
                       task_info->max_rank = (max == -1) ? max_ranks_default : max;
                       task_info->local_rank = task_rank - min;
                       task_info->num_lrank = task_info->max_rank - min;

                       foundit = 1;

                       break;
               }
       }

       /* close the config file */
       fclose(ikmp);

       /* if we did not find the config file, set it to the default */
       if(foundit == 0) {
           fprintf(stderr,
                   "ERROR: Unable to find iolang workload file "
                   "from given metadata file %s... exiting\n",
                   io_kernel_meta_path);
           exit(1);
       }

       return;
}

int codes_kernel_helper_parse_input(CodesIOKernel_pstate * ps, CodesIOKernelContext * c,
                          codeslang_inst * inst)
{
    int yychar;
    int status;
    int codes_inst = CL_NOOP;

    /* swap in the symbol table for the current LP context */
    CodesIOKernelScannerSetSymTable(c);

    do
    {
        c->locval = CodesIOKernel_get_lloc(*((yyscan_t *)c->scanner_));
        yychar = CodesIOKernel_lex((codesYYType*)c->lval, (YYLTYPE*)c->locval, c->scanner_);
        c->locval = NULL;
      
        /* for each instructions */
        switch(yychar)
        {
            /* for each instrunction that triggers a simulator event */
            case WRITEAT:
            case READAT:
            case GETGROUPRANK:
            case GETGROUPSIZE:
            case CLOSE:
            case OPEN:
            case SYNC:
            case SLEEP:
            case EXIT:
            case DELETE:
            {
                c->inst_ready = 0;
                status = CodesIOKernel_push_parse(ps, yychar, (codesYYType*)c->lval, (YYLTYPE*)c->locval, c);
                codes_inst = convertKLInstToEvent(yychar);
		break;
            }
            /* not a simulator event */
            default:
            {
                status = CodesIOKernel_push_parse(ps, yychar, (codesYYType*)c->lval, (YYLTYPE*)c->locval, c);
		break;
            }
        };

        /* if the instruction is ready (all preq data is ready ) */
        if(c->inst_ready)
        {
            /* reset instruction ready state */
            c->inst_ready = 0;

            switch(codes_inst)
            {
                case CL_GETRANK:
                case CL_GETSIZE:
                case CL_WRITEAT:
	        case CL_READAT:
                case CL_OPEN:
                case CL_CLOSE:
                case CL_SYNC:
                case CL_EXIT:
                case CL_DELETE:
                case CL_SLEEP:
                {
                    int i = 0;

                    inst->event_type = codes_inst;
                    inst->num_var = c->var[0];
                    for(i = 0 ; i < inst->num_var ; i++)
                    {
                        inst->var[i] = c->var[i + 1];
                    }

		    break;
                }
                /* we don't need to store the instructions args */
                default:
                {
                    continue;
                }
            };
            
	    /* we have all of the data for the instruction... bail */
            break;
        }
    /* while there are more instructions to parse in the stream */
    }while(status == YYPUSH_MORE);

    /* return the simulator instruction */
    return codes_inst;
}

int codes_kernel_helper_bootstrap(char * io_kernel_path,
        char * io_kernel_meta_path, int rank, int num_ranks, int use_relpath,
        CodesIOKernelContext * c, CodesIOKernel_pstate ** ps,
        codes_workload_info * task_info, codeslang_inst * next_event)
{
    int t = CL_NOOP;
    int ret = 0;
    char * kbuffer = NULL;
    int fd = 0;
    off_t ksize = 0;
    struct stat info;

    temp_group_rank = rank;
    /* get the kernel from the file */
    codes_kernel_helper_parse_cf(io_kernel_path,
            io_kernel_meta_path, rank, num_ranks, task_info, use_relpath);
    temp_group_size = task_info->num_lrank;
    /* stat the kernel file */
    ret = stat(io_kernel_path, &info);
    if(ret != 0)
    {
        fprintf(stderr, "%s:%i could not stat kernel file (%s), exiting\n",
                __func__, __LINE__, io_kernel_path);
        perror("stat() error: ");
        exit(1);
    }

    /* get the size of the file */
    ksize = info.st_size;

    /* allocate a buffer for the kernel */
    kbuffer = (char *)malloc(sizeof(char) * (ksize+1));
    kbuffer[ksize] = '\0';

    /* get data from the kernel file */
    fd = open(io_kernel_path, O_RDONLY);
    ret = pread(fd, kbuffer, ksize, 0);
    close(fd);

    /* init the scanner */
    CodesIOKernelScannerInit(c);

    if(ret <= ksize)
    {
        CodesIOKernel__scan_string(kbuffer, c->scanner_);
	
	*ps = CodesIOKernel_pstate_new();

        /* get the first instruction */
        t = codes_kernel_helper_parse_input(*ps, c, next_event);
    }
    else
    {
        fprintf(stderr, "not enough buffer space... bail\n");
        exit(1);
    }

    /* cleanup */
    free(kbuffer);

    return t;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
