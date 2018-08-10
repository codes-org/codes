import sys
import random

alloc_file = 'allocation.conf'
if len(sys.argv) > 2:
    alloc_file = sys.argv[2]

def contiguous_alloc(job_ranks, total_nodes):
    f = open(alloc_file,'w')
    start=0
    for num_rank in range(len(job_ranks)):
        for rankid in range(start, start+job_ranks[num_rank]):
            f.write("%s " % rankid)
        f.write("\n")
        start += job_ranks[num_rank]
    f.closed

def cube_alloc(job_ranks, total_nodes):
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



def permeate_alloc(job_ranks, total_nodes):
    f = open(alloc_file,'w')
    start=0
    node_list = range(0, int(total_nodes))
    random.seed(0)
    for num_rank in range(len(job_ranks)):
        permeate_area = job_ranks[num_rank]*4
        permeate_list = node_list[num_rank*permeate_area: (num_rank+1)*permeate_area]
        alloc_list = random.sample(permeate_list, job_ranks[num_rank])
        alloc_list.sort()
        for idx in range(len(alloc_list)):
            f.write("%s " % alloc_list[idx])
        f.write("\n")
    f.closed

def random_alloc(job_rank, total_nodes):
    f = open(alloc_file, 'w')
    node_list = range(0, int(total_nodes))
    random.seed(0)
    for rankid in range(len(job_rank)):
        alloc_list = random.sample(node_list, job_rank[rankid])
        node_list = [i for i in node_list if (i not in alloc_list)]
        for idx in range(len(alloc_list)):
            f.write("%s " % alloc_list[idx])
        f.write("\n")
    f.closed


def stripe_alloc(job_ranks, total_nodes):
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

def policy_select(plcy, job_ranks, total_nodes):
    if plcy == "CONT":
        print("contiguous alloction!")
        contiguous_alloc(job_ranks,  total_nodes)
    elif plcy == "rand":
        print("random allocation!")
        random_alloc(job_ranks, total_nodes)
    elif plcy == "STRIPE":
        print("stripe allcation!")
        stripe_alloc(job_ranks, total_nodes)
    elif plcy == "PERMEATE":
        print("permeate allocation!")
        permeate_alloc(job_ranks, total_nodes)
    elif plcy == "CUBE":
        print("cube allocation!")
        cube_alloc(job_ranks, total_nodes)
    else:
        print("NOT Supported yet!")


if __name__ == "__main__":
    f = open(sys.argv[1], "r")
    array = []

    for line in f:
        for number in line.split():
            array.append(number);

    f.close()
    alloc_plcy = array.pop(0)
    total_nodes = array.pop(0)
    print(alloc_plcy)
    array = map(int, array)
    print(array)
    policy_select(alloc_plcy, array, total_nodes)

