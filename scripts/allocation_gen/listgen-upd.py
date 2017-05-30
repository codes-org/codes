import sys
import random
alloc_file = 'testrest.conf'

def contiguous_alloc(job_ranks, total_nodes, cores_per_node):
    f = open(alloc_file,'w')
    start=0
    for num_rank in range(len(job_ranks)):
        for rankid in range(start, start+job_ranks[num_rank]):
            f.write("%s " % rankid)
        f.write("\n")
        start += job_ranks[num_rank]
    f.closed

def cube_alloc(job_ranks, total_nodes, cores_per_node):
    job_dim = [6,6,6]
    sys_dim_x = 16
    sys_dim_y =16
    sys_dim_z = 8
    cube = []
    start = 0
    for k in range(job_dim[2]):
        layer = []
        layer_offset = k*sys_dim_x*sys_dim_y
        for j in range(job_dim[1]):
            row_offset = j*sys_dim_z
            row = []
            for i in range(job_dim[0]):
                offset = row_offset+layer_offset
                row.append(i+offset)
            layer += row
        cube += layer
    print "list length is", len(cube), cube

    f = open('cube_allc_linear.conf','w')
    for rankid in range(len(cube)):
        f.write("%s " % cube[rankid])
    f.write("\n")
    f.closed

    f = open('cube_allc_random.conf','w')
    random.shuffle(cube)
    for rankid in range(len(cube)):
        f.write("%s " % cube[rankid])
    f.write("\n")
    f.closed



def permeate_alloc(job_ranks, total_nodes, cores_per_node):
    f = open(alloc_file,'w')
    start=0
    node_list = range(0, int(total_nodes))
    random.seed(0)
    for num_rank in range(len(job_ranks)):
        permeate_area = job_ranks[num_rank]*4
        permeate_list = node_list[num_rank*permeate_area: (num_rank+1)*permeate_area]
        alloc_list = random.sample(permeate_list, job_ranks[num_rank])
        alloc_list.sort()
        print "length of alloc list", len(alloc_list), "\n", alloc_list,"\n"
        for idx in range(len(alloc_list)):
            f.write("%s " % alloc_list[idx])
        f.write("\n")
    f.closed

def random_alloc(job_rank, total_nodes, num_seed, cores_per_node):
    filename_substr='rand_node1-alloc-'+str(total_nodes)+'-'
    for jobsize in job_rank:
        filename_substr += str(jobsize)+'_'
        #print filename_substr
    filename_substr=filename_substr[:-1]
    for seed in range(num_seed):
        filename_substr += '-'+str(seed)
        #print filename_substr
        f = open(filename_substr+'.conf', 'w')
        node_list = range(0, int(total_nodes))
        random.seed(seed)
        
        for rankid in range(len(job_rank)):
            rem = job_rank[rankid]/cores_per_node
            if(job_rank[rankid] % cores_per_node):
                rem=rem+1

            alloc_list = random.sample(node_list, rem)
            node_list = [i for i in node_list if (i not in alloc_list)]
            #print "length of alloc list", len(alloc_list), "\n", alloc_list,"\n"
            count = 0
            for idx in range(len(alloc_list)):
                for coreid in range(cores_per_node):
                    if(count == job_rank[rankid]):
                        break
                    f.write("%s " % ((alloc_list[idx] * cores_per_node) + coreid))
                    count=count+1

            f.write("\n")
        f.closed
        filename_substr=filename_substr[:-2]

def hybrid_alloc(job_rank, total_nodes, cores_per_node):
    #1st job get contiguous allocation , the other job get random allocation
    f = open(alloc_file, 'w')
    node_list = range(0, int(total_nodes))
    random.seed(0)
    group_size = 32
    for rankid in range(len(job_rank)):
        if(rankid == 0):
            job_size = job_rank[rankid]
            num_groups = job_size/group_size +1;#job_0 will take this number of groups
            alloc_list = node_list[0: job_rank[rankid]]
            node_list = node_list[num_groups*group_size : ]
        else:
            alloc_list = random.sample(node_list, job_rank[rankid])

        node_list = [i for i in node_list if (i not in alloc_list)]
        print "length of alloc list", len(alloc_list), "\n", alloc_list,"\n"
        for idx in range(len(alloc_list)):
            f.write("%s " % alloc_list[idx])
        f.write("\n")
    f.closed

def hybrid_alloc_2 (job_rank, total_nodes, cores_per_node):
    #1st and 2nd job get contiguous allocation , 3rd job get random allocation
    f = open(alloc_file, 'w')
    node_list = range(0, int(total_nodes))
    random.seed(0)
    group_size = 32
    for rankid in range(len(job_rank)):
        #if(rankid !=2 2 ):
        if(rankid < 3 ):#all job get contiguous allocation
            job_size = job_rank[rankid]
            num_groups = job_size/group_size +1;#job_0 and job_1 will take this number of groups
            alloc_list = node_list[0: job_rank[rankid]]
            node_list = node_list[num_groups*group_size : ]
        else:
            alloc_list = random.sample(node_list, job_rank[rankid])

        node_list = [i for i in node_list if (i not in alloc_list)]
        print "length of alloc list", len(alloc_list), "\n", alloc_list,"\n"
        for idx in range(len(alloc_list)):
            f.write("%s " % alloc_list[idx])
        f.write("\n")
    f.closed



def stripe_alloc(job_ranks, total_nodes, cores_per_node):
    #print "the num of nodes of each Job", job_ranks
    f = open(alloc_file,'w')
    node_list = range(0, int(total_nodes))
    stripe_size = 2
    alloc_list = []
    for num_rank in range(len(job_ranks)):
    #    print "job id", num_rank
        num_stripe = 1
        start = num_rank*stripe_size
        if(job_ranks[num_rank] % stripe_size != 0):
            num_stripe = job_ranks[num_rank]/stripe_size+1
        else:
            num_stripe = job_ranks[num_rank]/stripe_size
        tmp_list = []
        while(num_stripe>0):
            tmp_list += node_list[start:start+stripe_size]
            start += len(job_ranks)*stripe_size
            num_stripe -= 1
        alloc_list.append(tmp_list)


    for job_id in range (len(alloc_list)):
        tmp = alloc_list[job_id]
        #print "alloc list for JOB", job_id
        for rankid in range (job_ranks[job_id]):
           # print tmp[rankid]
            f.write("%s " % tmp[rankid])
        f.write("\n")
    f.closed

def policy_select(plcy, job_ranks, total_nodes, num_seed, cores_per_node):
    if plcy == "CONT":
        print "contiguous alloction!"
        contiguous_alloc(job_ranks,  total_nodes, cores_per_node)
    elif plcy == "rand":
        print "random allocation!"
        random_alloc(job_ranks, total_nodes, num_seed, cores_per_node)
    elif plcy == "STRIPE":
        print "stripe allcation!"
        stripe_alloc(job_ranks, total_nodes, cores_per_node)
    elif plcy == "PERMEATE":
        print "permeate allocation!"
        permeate_alloc(job_ranks, total_nodes, cores_per_node)
    elif plcy == "CUBE":
        print "cube allocation!"
        cube_alloc(job_ranks, total_nodes, cores_per_node)
    elif plcy == "hybrid":
        print "hybrid allocation!"
        hybrid_alloc(job_ranks, total_nodes, cores_per_node)
    elif plcy == "hybrid-2":
        print "hybrid 2 allocation!"
        hybrid_alloc_2(job_ranks, total_nodes, cores_per_node)
    else:
        print "NOT Supported yet!"


if __name__ == "__main__":
    f = open(sys.argv[1], "r")
    array = []

    for line in f:
        for number in line.split():
            array.append(number);

    f.close()
    alloc_plcy = array.pop(0)
    total_nodes = array.pop(0)
    num_seed = int(array.pop(0))
    cores_per_node = int(array.pop(0))
    print alloc_plcy
    print cores_per_node
    array = map(int, array)
    #print array
    #print num_seed, type(num_seed)
    policy_select(alloc_plcy, array, total_nodes, num_seed, cores_per_node)

