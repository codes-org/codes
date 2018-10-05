import string
import sys
import os

MPI_OPS = [ 'MPI_Send', 'MPI_Recv', 'MPI_Barrier', 'MPI_Isend', 'MPI_Irecv', 'MPI_Waitall', 
            'MPI_Reduce', 'MPI_Allreduce', 'MPI_Bcast', 'MPI_Alltoall', 'MPI_Alltoallv',
            'MPI_Comm_size', 'MPI_Comm_rank']

LOG = [ 'logfiletmpl_default', 'ncptl_log_write', 'ncptl_log_compute_aggregates', 'ncptl_log_commit_data']

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

def eliminate_conc_init(inLines):
    for idx, line in enumerate(inLines):
        if 'NCPTL_RUN_TIME_VERSION' in line:
            inLines[idx] = "//"+line
        if 'atexit (conc_exit_handler)' in line:
            inLines[idx] = "//"+line
        if 'Inform the run-time library' in line:
            for i in range(1, 4):
                inLines[idx+i] = "//"+inLines[idx+i]

def make_static_var(inLines):
    for idx, line in enumerate(inLines):
        if 'Dummy variable to help mark other variables as used' in line:
            inLines[idx+1]="static " + inLines[idx+1]
        if 'void conc_mark_variables_used' in line:
            inLines[idx]="static " + line
        if '/* Program-specific variables */' in line:
            start = idx+1
        if '* Function declarations *' in line:
            end = idx-2

    for i in range(start, end):
        inLines[i]="static "+inLines[i]


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
            elif 'MPI_Comm_get_attr' in line:
                inLines[idx] = "//"+line
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

    # adding struct at the end
    for i in range(0, len(new_struct)):
        inLines.append(new_struct[i])


def insert_if_not_exist(content, idx, hls):
    exist = False
    for i in range(idx[0], idx[1]):
        if hls[i] in content:
            exist = True
            break

    if not exist:
        hls.insert(idx[0], content)

def translate_conc_to_codes(filepath, codespath):
    # get program name
    program_name = filepath.split("/")[-1].replace(".c","")

    with open(filepath, 'r') as infile:
        content = infile.read()
    inLines = content.split('\n')

    eliminate_logging(inLines)
    eliminate_conc_init(inLines)
    make_static_var(inLines)
    manipulate_mpi_ops(inLines, program_name)
    adding_struct(inLines, program_name)

    # output program file
    with open(codespath+"src/workload/conceputal-skeleton-apps/conc-"+program_name+".c","w+") as outFile:
        outFile.writelines(["%s\n" % item for item in inLines])

    # modify interface file
    program_struct = "extern struct codes_conceptual_bench "+program_name+"_bench;\n"
    program_struct_idx=[]
    program_definition = "    &"+program_name+"_bench,\n"
    program_definition_idx=[]
    with open(codespath+"src/workload/codes-conc-addon.c","r+") as header:
        hls = header.readlines()
        for idx, line in enumerate(hls):
            if '/* list of available benchmarks begin */' in line:
                program_struct_idx.append(idx+1)
            elif '/* list of available benchmarks end */' in line:
                program_struct_idx.append(idx)
        insert_if_not_exist(program_struct, program_struct_idx, hls)

        for idx, line in enumerate(hls):
            if '/* default benchmarks begin */' in line:
                program_definition_idx.append(idx+1)
            elif '/* default benchmarks end */' in line:
                program_definition_idx.append(idx)
        insert_if_not_exist(program_definition, program_definition_idx, hls)

        header.seek(0)
        header.writelines(hls)

    # modify makefile
    program_compile = "src_libcodes_la_SOURCES += src/workload/conceputal-skeleton-apps/conc-"+program_name+".c\n"
    program_compile_idx = []
    with open(codespath+"Makefile.am","r+") as makefile:
        mfls = makefile.readlines()
        for idx, line in enumerate(mfls):
            if "CONCEPTUAL_LIBS" in line:
                program_compile_idx.append(idx+1)
                break
        for i in range(program_compile_idx[0], len(mfls)):
            if 'endif' in mfls[i]:
                program_compile_idx.append(i)
                break
        insert_if_not_exist(program_compile, program_compile_idx, mfls)        
        makefile.seek(0)
        makefile.writelines(mfls)        


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print 'Need 2 arguments: 1. path to files to be converted \t2. path to CODES directory\t3. path to ncptl executable'
        sys.exit(1)
    
    os.chdir(sys.argv[1])
    for benchfile in next(os.walk(sys.argv[1]))[2]:    # for all files
        if benchfile.lower().endswith('.ncptl'):
            cfile = benchfile.replace('.ncptl','.c')
            cfile = cfile.replace("-","")
            os.system(sys.argv[3]+' --backend=c_mpi --no-compile '+benchfile+' --output '+cfile)
            print "adding bench file: %s" % cfile
            translate_conc_to_codes(sys.argv[1]+cfile, sys.argv[2])




