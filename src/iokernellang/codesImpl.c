#include <stdio.h>
#include <stdlib.h>
#include "codes/CodesIOKernelTypes.h"
#include "codes/CodesIOKernelParser.h"

int64_t ex(
    nodeType * p)
{
    if(!p)
    {
        return 0;
    }

    /* value or op switch */
    switch (p->type)
    {
        case typeCon:
        {
            return p->con.value;
        }
        case typeId:
        {
            return sym[p->id.i];
        }
        case typeOpr:
        {
            /* op switch */
            switch (p->opr.oper)
            {
                case WHILE:
                {
                    while(ex(p->opr.op[0]))
                    {
                        ex(p->opr.op[1]);
                    }
                    return 0;
                }
                case IF:
                {
                    if(ex(p->opr.op[0]))
                    {
                        ex(p->opr.op[1]);
                    }
                    else if(p->opr.nops > 2)
                    {
                        ex(p->opr.op[2]);
                    }
                    return 0;
                }
                case PRINT:
                {
                    printf("%ld\n", ex(p->opr.op[0]));
                    fflush(stdout);
                    return 0;
                }
                case WRITE_ALL:
                {
                    int64_t t1 = ex(p->opr.op[0]);
                    int64_t t2 = ex(p->opr.op[1]);

                    /* local storage of data used for the op */
                    var[0] = 2;
                    var[1] = t1;
                    var[2] = t2;
                    *inst_ready = 1;

                    return 0;
                }
                case WRITEAT_ALL:
                {
                    int64_t t1 = ex(p->opr.op[0]);
                    int64_t t2 = ex(p->opr.op[1]);
                    int64_t t3 = ex(p->opr.op[2]);

                    /* local storage of data used for the op */
                    var[0] = 3;
                    var[1] = t1;
                    var[2] = t2;
                    var[3] = t3;
                    *inst_ready = 1;

                    return 0;
                }
                case WRITE:
                {
                    int64_t t1 = ex(p->opr.op[0]);
                    int64_t t2 = ex(p->opr.op[1]);

                    /* local storage of data used for the op */
                    var[0] = 2;
                    var[1] = t1;
                    var[2] = t2;
                    *inst_ready = 1;

                    return 0;
                }
                case WRITEAT:
                {
                    int64_t t1 = ex(p->opr.op[0]);
                    int64_t t2 = ex(p->opr.op[1]);
                    int64_t t3 = ex(p->opr.op[2]);

                    /* local storage of data used for the op */
                    var[0] = 3;
                    var[1] = t1;
                    var[2] = t2;
                    var[3] = t3;
                    *inst_ready = 1;

                    return 0;
                }
                case READ_ALL:
                {
                    int64_t t1 = ex(p->opr.op[0]);
                    int64_t t2 = ex(p->opr.op[1]);

                    /* local storage of data used for the op */
                    var[0] = 2;
                    var[1] = t1;
                    var[2] = t2;
                    *inst_ready = 1;

                    return 0;
                }
                case READAT_ALL:
                {
                    int64_t t1 = ex(p->opr.op[0]);
                    int64_t t2 = ex(p->opr.op[1]);
                    int64_t t3 = ex(p->opr.op[2]);

                    /* local storage of data used for the op */
                    var[0] = 3;
                    var[1] = t1;
                    var[2] = t2;
                    var[3] = t3;
                    *inst_ready = 1;

                    return 0;
                }
                case READ:
                {
                    int64_t t1 = ex(p->opr.op[0]);
                    int64_t t2 = ex(p->opr.op[1]);

                    /* local storage of data used for the op */
                    var[0] = 2;
                    var[1] = t1;
                    var[2] = t2;
                    *inst_ready = 1;

                    return 0;
                }
                case READAT:
                {
                    int64_t t1 = ex(p->opr.op[0]);
                    int64_t t2 = ex(p->opr.op[1]);
                    int64_t t3 = ex(p->opr.op[2]);

                    /* local storage of data used for the op */
                    var[0] = 3;
                    var[1] = t1;
                    var[2] = t2;
                    var[3] = t3;
                    *inst_ready = 1;

                    return 0;
                }
                case SYNC:
                {
                    int64_t t1 = ex(p->opr.op[0]);

                    /* local storage of data used for the op */
                    var[0] = 1;
                    var[1] = t1;
                    *inst_ready = 1;

                    return 0;
                }
                case SLEEP:
                {
                    int64_t t1 = ex(p->opr.op[0]);

                    /* local storage of data used for the op */
                    var[0] = 1;
                    var[1] = t1;
                    *inst_ready = 1;

                    return 0;
                }
                case OPEN:
                {
                    int64_t t1 = ex(p->opr.op[0]);

                    /* local storage of data used for the op */
                    var[0] = 1;
                    var[1] = t1;
                    *inst_ready = 1;

                    return 0;
                }
                case CLOSE:
                {
                    int64_t t1 = ex(p->opr.op[0]);

                    /* local storage of data used for the op */
                    var[0] = 1;
                    var[1] = t1;
                    *inst_ready = 1;

                    return 0;
                }
                case DELETE:
                {
                    int64_t t1 = ex(p->opr.op[0]);

                    /* local storage of data used for the op */
                    var[0] = 1;
                    var[1] = t1;
                    *inst_ready = 1;

                    return 0;
                }
                case FLUSH:
                {
                    int64_t t1 = ex(p->opr.op[0]);

                    /* local storage of data used for the op */
                    var[0] = 1;
                    var[1] = t1;
                    *inst_ready = 1;

                    return 0;
                }
                case SEEK:
                {
                    int64_t t1 = ex(p->opr.op[0]);
                    int64_t t2 = ex(p->opr.op[0]);

                    /* local storage of data used for the op */
                    var[0] = 1;
                    var[1] = t1;
                    var[2] = t2;
                    *inst_ready = 1;

                    return 0;
                }
                case GETNUMGROUPS:
                {
                    return 32;
                }
                case GETGROUPID:
                {
                    return 8;
                }
                case EXIT:
                {
                    int64_t t1 = ex(p->opr.op[0]);
                    var[0] = 1;
                    var[1] = t1;
                    *inst_ready = 1;

                    return 4;
                }
                case GETGROUPRANK:
                {
                    int64_t t1 = ex(p->opr.op[0]);
                    var[0] = 1;
                    var[1] = t1;
                    *inst_ready = 1;

                    return *group_rank;
                }
                case GETGROUPSIZE:
                {
                    int64_t t1 = ex(p->opr.op[0]);
                    var[0] = 1;
                    var[1] = t1;
                    *inst_ready = 1;

                    return *group_size;
                }
                case GETCURTIME:
                {
                    return 0;
                }
                case ';':
                {
                    ex(p->opr.op[0]);
                    return ex(p->opr.op[1]);
                }
                case '=':
                {
                    return sym[p->opr.op[0]->id.i] = ex(p->opr.op[1]);
                }
                case UMINUS:
                {
                    return -ex(p->opr.op[0]);
                }
                case '+':
                {
                    return ex(p->opr.op[0]) + ex(p->opr.op[1]);
                }
                case '-':
                {
                    return ex(p->opr.op[0]) - ex(p->opr.op[1]);
                }
                case '*':
                {
                    return ex(p->opr.op[0]) * ex(p->opr.op[1]);
                }
                case '/':
                {
                    return ex(p->opr.op[0]) / ex(p->opr.op[1]);
                }
                case '%':
                {
                    return ex(p->opr.op[0]) % ex(p->opr.op[1]);
                }
                case '<':
                {
                    return ex(p->opr.op[0]) < ex(p->opr.op[1]);
                }
                case '>':
                {
                    return ex(p->opr.op[0]) > ex(p->opr.op[1]);
                }
                case GE:
                {
                    return ex(p->opr.op[0]) >= ex(p->opr.op[1]);
                }
                case LE:
                {
                    return ex(p->opr.op[0]) <= ex(p->opr.op[1]);
                }
                case NE:
                {
                    return ex(p->opr.op[0]) != ex(p->opr.op[1]);
                }
                case EQ:
                {
                    return ex(p->opr.op[0]) == ex(p->opr.op[1]);
                }
            }
        }
    }
    return 0;
}
