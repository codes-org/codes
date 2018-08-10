import string
import sys
import os

MPI_OPS = [ 'MPI_Send', 'MPI_Recv', 'MPI_Barrier', 'MPI_Isend', 'MPI_Irecv', 'MPI_Waitall', 
            'MPI_Reduce', 'MPI_Allreduce', 'MPI_Bcast', 'MPI_Alltoall', 'MPI_Alltoallv',
            'MPI_Comm_size', 'MPI_Comm_rank']

LOG = [ 'logfiletmpl_default', 'ncptl_log_compute_aggregates', 'ncptl_log_commit_data']

def eliminate_logging(inLines):
    for idx, line in enumerate(inLines):
        if 'Generate and broadcast a UUID' in line:
            for i in range(1, 3):
                inLines[idx+i] = "//"+inLines[idx+i]  
        elif 'ncptl_free (logfile_uuid)' in line:
            for i in range(0, 12):
                inLines[idx-i] = "//"+inLines[idx-i]
        elif 'case 1' in line:
            for i in range(5, 9):
                inLines[idx+i] = "//"+inLines[idx+i]      
        elif 'int mpiresult' in line:
            for i in range(0,30):
                inLines[idx+i] = "//"+inLines[idx+i] 
        else:
            for elem in LOG:
                if elem in line:
                    inLines[idx] = "//"+line


def manipulate_mpi_ops(inLines, program_name):
    for idx, line in enumerate(inLines):
        # subcomm
        if 'MPI_' not in line:  # not MPI
            if "int main" in line:
                # inLines[idx] = "static int "+program_name+"_main(int* argc, char *argv[])"
                inLines[idx] = line.replace("int main", "static int "+program_name+"_main")
            else:
                continue
        else:   # MPI 
            if 'MPI_Init' in line:
                inLines[idx] = "//"+line
            elif 'MPI_Errhandler_' in line:     # error handling ignored
                inLines[idx] = "//"+line
            elif 'mpiresult = MPI_Finalize();' in line:
                inLines[idx] = "CODES_MPI_Finalize();"
                inLines[idx+2] = "exitcode = 0;"
            else:
                for ops in MPI_OPS:
                    if ops in line:
                        inLines[idx] = line.replace(ops,"CODES_"+ops)

def adding_struct(inLines, program_name):
    new_struct = [ '/* fill in function pointers for this method */' ,
                   'struct codes_conceptual_bench '+program_name+'_bench = ' , 
                   '{' ,
                   '.program_name = "'+program_name+'",' ,
                   '.conceptual_main = '+program_name+'_main,' ,
                   '};' ]

    codes_include = '#include "codes/codes-conc-addon.h"'
    for idx, line in enumerate(inLines):
        if "* Include files *" in line:
            inLines.insert(idx-1, codes_include)
            break

    for idx, line in enumerate(inLines):
        if "* Global variables *" in line:
            for i in range(len(new_struct)-1,-1,-1):
                inLines.insert(idx-1, new_struct[i])
            break
            

def translate_conc_to_codes(filepath, codespath):
    # get program name
    program_name = filepath.split("/")[-1].replace(".c","")

    with open(filepath, 'r') as infile:
        content = infile.read()
    # print content
    inLines = content.split('\n')

    eliminate_logging(inLines)
    manipulate_mpi_ops(inLines, program_name)
    adding_struct(inLines, program_name)

    # output program file
    with open(codespath+"src/workload/methods/conc-"+program_name+".c","w+") as outFile:
        outFile.writelines(["%s\n" % item for item in inLines])

    # modify interface file
    program_struct = "extern struct codes_conceptual_bench "+program_name+"_bench;\n"
    program_definition = "    &"+program_name+"_bench,\n"
    with open(codespath+"src/workload/codes-conc-addon.c","r+") as header:
        hls = header.readlines()
        for idx, line in enumerate(hls):
            if '/* list of available benchmarks begin */' in line and program_struct not in hls[idx+1]:
                hls.insert(idx+1, program_struct)
            elif '/* default benchmarks begin */' in line and program_definition not in hls[idx+1]:
                hls.insert(idx+1, program_definition)
        header.seek(0)
        header.writelines(hls)

    # modify makefile
    program_compile = "src_libcodes_la_SOURCES += src/workload/methods/conc-"+program_name+".c\n"
    with open(codespath+"Makefile.am","r+") as makefile:
        mfls = makefile.readlines()
        for idx, line in enumerate(mfls):
            if "CONCEPTUAL_LIBS" in line and program_compile not in mfls[idx+1]:
                mfls.insert(idx+1, program_compile)
                break
        makefile.seek(0)
        makefile.writelines(mfls)        


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print 'Need 2 arguments: 1. path to files to be converted \t2. path to CODES directory'
        sys.exit(1)
    
    for benchfile in next(os.walk(sys.argv[1]))[2]:    # for all files
        translate_conc_to_codes(sys.argv[1]+benchfile, sys.argv[2])




