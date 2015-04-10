/***************
  Copyright (c) 2015, MedicineYeh
  Copyright (c) 2013, Matthew Levenstein
  All rights reserved.

  Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

  Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
  Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *****************/

/*
 * To conver to an append-only design, only the 'fwrite' in the '_insert' function
 * must be changed. All other writes (save for delete), are appended to the end.
 * Also, the root address must instead be written to and searched for at the end.
 */

#define DATA_SIZE 10000
#include <stdio.h>
#include <stdlib.h>    
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <CL/cl.h>

#ifdef L_ctermidNOPE
#include <unistd.h>
#include <fcntl.h>
#define LOCK
#endif

#define checkErr(fun, statement) err = fun;\
                                 if (err != CL_SUCCESS) {statement}
#define checkExit(value, message) if (value == 0) {printf(message); goto release;}

typedef struct db db;

#define SIZEOF_LONG sizeof(uint64_t)
#define _HASH 128
#define _ORDER 99
#define _WIDTH (1+_HASH*_ORDER+SIZEOF_LONG*(_ORDER+1))
#define _DEPTH 10
#define _MAX 0xf4240

struct db{
    FILE* fp;
    unsigned char path[_DEPTH][_WIDTH];
    uint64_t node_addrs[_DEPTH];
#ifdef LOCK
    struct flock fl;
#endif
};

void to_big(unsigned char *, uint64_t);
uint64_t from_big(unsigned char *);
void node_split(db *, int, unsigned char);
void _insert(db *, unsigned char *, int, uint64_t, uint64_t, int);
void db_put(db *, unsigned char *, unsigned char *);
uint64_t db_search(db *, unsigned char *, int *);
unsigned char* db_get(db *, unsigned char *);
void db_init(db *, const char *);
void db_delete(db *, unsigned char*);
unsigned char *read_data(FILE *fp);
uint64_t db_search_in_mem(db *, unsigned char *, int *);
unsigned char *data = NULL;

#ifdef LOCK
int db_lock(db *);
int db_unlock(db *);
#endif

void db_close(db *);
void print_usage(void);
cl_program load_program(cl_context, cl_device_id, const char *);

cl_int err = 0;
cl_uint num = 0;
cl_platform_id *platforms = NULL;
cl_context_properties prop[3] = {0};
cl_context context = 0;
cl_device_id *devices = NULL;
cl_command_queue queue = 0;
cl_program program = 0;
cl_mem cl_a = 0, cl_b = 0, cl_c = 0, cl_d = 0, cl_e = 0, cl_f = 0;
cl_kernel adder = 0;
int data_length = 0;

#ifdef LOCK
int db_lock(db* db) {
    db->fl.l_type   = F_WRLCK; 
    db->fl.l_whence = SEEK_SET; 
    db->fl.l_start  = 0;        
    db->fl.l_len    = 0;        
    db->fl.l_pid    = getpid();
    return fcntl((db->fp)->_file,F_SETLKW, &(db->fl));
}

int db_unlock(db* db) {
    db->fl. l_type = F_UNLCK;
    fcntl((db->fp)->_file, F_SETLK, &(db->fl));
}
#endif

void to_big(unsigned char* buf, uint64_t val) {
    int i;
    for( i=0; i<sizeof(uint64_t); ++i )
        buf[i] = (val >> (56-(i*8))) & 0xff;
}

uint64_t from_big(unsigned char* buf) {
    uint64_t val = 0;
    int i;
    for( i=0; i<sizeof(uint64_t); ++i )
        val |= (uint64_t) buf[i] << (56-(i*8));
    return val;
}

void node_split(db* db, int index, unsigned char isleaf)
{
    unsigned char* node = db->path[index];
    unsigned char* lnode = malloc(_WIDTH+1);
    unsigned char* rnode = malloc(_WIDTH+1);
    memset(lnode,0,_WIDTH+1);
    memset(rnode,0,_WIDTH+1);
    int split = (_HASH+SIZEOF_LONG)*(_ORDER>>1)+1;
    memcpy(lnode,node,split);
    rnode[0] = isleaf;
    isleaf ? 
        memcpy(rnode+1,node+split,_WIDTH-split) :
        memcpy(rnode+1,node+split+SIZEOF_LONG+_HASH,_WIDTH-(split+SIZEOF_LONG+_HASH));
    fseeko(db->fp,0,SEEK_END);
    uint64_t addr = ftello(db->fp);
    fwrite(rnode,1,_WIDTH,db->fp);
    to_big(lnode+split,addr);
    unsigned char* key = malloc(_HASH+1);
    memset(key,0,_HASH+1);
    memcpy(key,node+split+SIZEOF_LONG,_HASH);
    memcpy(db->path[index],lnode,_WIDTH);
    if( index > 0 ){
        _insert(db,key,index-1,db->node_addrs[index],addr,0);
    }
    else{
        unsigned char* root = malloc(_WIDTH+1);
        memset(root,0,_WIDTH+1);
        root[0] = 0;
        to_big(root+1,db->node_addrs[0]);
        strncpy(root+1+SIZEOF_LONG,key,_HASH);
        to_big(root+1+SIZEOF_LONG+_HASH,addr);
        fseeko(db->fp,0,SEEK_END);
        addr = ftello(db->fp);
        fwrite(root,1,_WIDTH,db->fp);
        fseeko(db->fp,0,SEEK_SET);
        fwrite(&addr,1,SIZEOF_LONG,db->fp);
    }
}

void _insert(db* db, unsigned char* key, int index, uint64_t addr, uint64_t rptr, int isleaf)
{
    if( _HASH > strlen(key) ){
        unsigned char* okey = key;
        key = malloc(_HASH+1);
        memset(key,0x61,_HASH+1);
        strncpy(key,okey,strlen(okey));
    }
    unsigned char* node = db->path[index];
    int i = SIZEOF_LONG+1;
    for( ; i<_WIDTH; i+=(_HASH+SIZEOF_LONG) ){
        if( node[i] == 0 ){
            i -= SIZEOF_LONG;
            to_big(node+i,addr);
            i += SIZEOF_LONG;
            strncpy(node+i,key,_HASH);
            if( !isleaf ){
                i += _HASH;
                to_big(node+i,rptr);
            }
            break;
        }
        if( !strncmp(node+i,key,_HASH) ){
            if( isleaf ){
                i -= SIZEOF_LONG;
                to_big(node+i,addr);
            }
            break;
        }
        if( strncmp(node+i,key,_HASH) > 0 ){
            unsigned char* nnode = malloc(_WIDTH+1);
            memset(nnode,0,_WIDTH+1);
            i -= SIZEOF_LONG;
            int j;
            for( j=0; j<i; ++j ){
                nnode[j] = node[j];
            }
            to_big(nnode+i,addr);
            i += SIZEOF_LONG;
            strncpy(nnode+i,key,_HASH);
            i += _HASH;
            if( !isleaf ){
                to_big(nnode+i,rptr);
                i += SIZEOF_LONG;
                j += SIZEOF_LONG;
            }
            for( ; i<_WIDTH; ++i, ++j ){
                nnode[i] = node[j];
            }
            memcpy(db->path[index],nnode,_WIDTH);
            break;
        }
    }
    if( node[(_WIDTH-(_HASH+SIZEOF_LONG))] != 0 ){
        isleaf ?
            node_split(db,index,1):
            node_split(db,index,0);
    }
    fseeko(db->fp,db->node_addrs[index],SEEK_SET);
    fwrite(db->path[index],1,_WIDTH,db->fp);
}

uint64_t db_search(db* db, unsigned char* key, int* r_index)
{
    if( _HASH > strlen(key) ){
        unsigned char* okey = key;
        key = malloc(_HASH+1);
        memset(key,0x61,_HASH+1);
        strncpy(key,okey,strlen(okey));
    }
    uint64_t r_addr;
    int i = SIZEOF_LONG+1;
    unsigned char isleaf;
    int index = 0;
    fseeko(db->fp,0,SEEK_SET);
    fread(&r_addr,1,SIZEOF_LONG,db->fp);
    fseeko(db->fp,r_addr,SEEK_SET);
    fread(db->path[index],1,_WIDTH,db->fp);
    db->node_addrs[index] = r_addr;
search:
    isleaf = db->path[index][0];
    for( ; i<_WIDTH; i+=(_HASH+SIZEOF_LONG) ){
        if( !strncmp(db->path[index]+i,key,_HASH) ){
            if( isleaf ){
                *r_index = index;
                i -= SIZEOF_LONG;
                uint64_t cindex = from_big(db->path[index]+i);
                fseeko(db->fp,cindex,SEEK_SET);
                unsigned char check;
                fread(&check,1,1,db->fp);
                if( check == 0 ){
                    return 1;
                }
                return 0;
            }
            if( index >= _DEPTH ){
                *r_index = 0;
                return -1;
            }
            i += _HASH;
            uint64_t addr = from_big(db->path[index]+i);
            fseeko(db->fp,addr,SEEK_SET);
            ++index;
            fread(db->path[index],1,_WIDTH,db->fp);
            db->node_addrs[index] = addr;
            i = SIZEOF_LONG+1;
            goto search;
        }
        if( strncmp(db->path[index]+i,key,_HASH) > 0 ||
                db->path[index][i] == 0 ){
            if( isleaf ){
                *r_index = index;
                return 1;
            }
            if( index >= _DEPTH ){
                *r_index = 0;
                return -1;
            }
            i -= SIZEOF_LONG;
            uint64_t addr = from_big(db->path[index]+i);
            fseeko(db->fp,addr,SEEK_SET);
            ++index;
            fread(db->path[index],1,_WIDTH,db->fp);
            db->node_addrs[index] = addr;
            i = SIZEOF_LONG+1;
            goto search;
        }
    }
}

void db_put(db* db, unsigned char* key, unsigned char* value)
{
    int index;
    uint64_t ret;
#ifdef LOCK
    if( db_lock(db) == -1 ){
        perror("fcntl");
        return;
    }
    else{
#endif
        if( (ret = db_search(db,key,&index)) > 0 ){
            uint64_t k_len = strlen(key);
            uint64_t v_len = strlen(value);
            if( k_len+v_len > _MAX ){ return; }
            uint64_t n_len = k_len+v_len+SIZEOF_LONG+SIZEOF_LONG+1;
            unsigned char* nnode = malloc(n_len+1);
            unsigned char* ptr = nnode;
            memset(nnode,0,n_len+1);
            nnode[0] = 1;
            to_big(ptr+1,k_len);
            strncpy(ptr+SIZEOF_LONG+1,key,k_len);
            to_big(ptr+SIZEOF_LONG+k_len+1,v_len);
            strncpy(ptr+SIZEOF_LONG+k_len+SIZEOF_LONG+1,value,v_len);
            fseeko(db->fp,0,SEEK_END);
            uint64_t addr = ftello(db->fp);
            fwrite(nnode,1,n_len,db->fp);
            _insert(db,key,index,addr,0,1);
        }
#ifdef LOCK
        if( db_unlock(db) == -1 ){
            perror("fcntl");
            return;
        }
    }
#endif
    fflush(db->fp);
}

unsigned char* db_get(db* db, unsigned char* key)
{
    int index;
    if( _HASH > strlen(key) ){
        unsigned char* okey = key;
        key = malloc(_HASH+1);
        memset(key,0x61,_HASH+1);
        strncpy(key,okey,strlen(okey));
    }
    if( !db_search_in_mem(db,key,&index) ){
        int i = SIZEOF_LONG+1, j;
        for( ; i<_WIDTH; i+=(SIZEOF_LONG+_HASH) ){
            if( !strncmp(db->path[index]+i,key,_HASH) ){
                i -= SIZEOF_LONG;
                uint64_t addr = from_big(db->path[index]+i);
                fseeko(db->fp,addr,SEEK_SET);
                unsigned char exists;
                unsigned char k_len[SIZEOF_LONG];
                unsigned char v_len[SIZEOF_LONG];
                fread(&exists,1,1,db->fp);
                if( !exists ){
                    return NULL;
                }
                fread(k_len,1,SIZEOF_LONG,db->fp);
                uint64_t k_lenb = from_big(k_len);
                unsigned char* k = malloc(k_lenb);
                memset(k,0,k_lenb);
                fread(k,1,k_lenb,db->fp);
                fread(v_len,1,SIZEOF_LONG,db->fp);
                uint64_t v_lenb = from_big(v_len);
                unsigned char* v = malloc(v_lenb);
                memset(v,0,v_lenb);
                fread(v,1,v_lenb,db->fp);
                return v;
            }
        }
        return NULL;
    }
    return NULL;
}

void db_delete(db* db, unsigned char* key)
{
    int index;
    if( _HASH > strlen(key) ){
        unsigned char* okey = key;
        key = malloc(_HASH+1);
        memset(key,0x61,_HASH+1);
        strncpy(key,okey,strlen(okey));
    }
    if( !db_search(db,key,&index) ){
        int i = SIZEOF_LONG+1;
        for( ; i<_WIDTH; i+=(SIZEOF_LONG+_HASH) ){
            if( !strncmp(db->path[index]+i,key,_HASH) ){
                i -= SIZEOF_LONG;
                unsigned char del = 0;
                uint64_t addr = from_big(db->path[index]+i);
                fseeko(db->fp,addr,SEEK_SET);
                fwrite(&del,1,1,db->fp);
            }
        }
    }
}

void db_init(db* db, const char* name)
{
    uint64_t addr;
    unsigned char* zero = malloc(_WIDTH);
    memset(zero,0,_WIDTH);
    zero[0] = 1;
    db->fp = fopen(name,"rb+");
    if(!db->fp){
        printf("%s not found, create a new one.\n", name);
        db->fp = fopen(name,"wb+");
        addr = SIZEOF_LONG;
        fseeko(db->fp,0,SEEK_SET);
        fwrite(&addr,SIZEOF_LONG,1,db->fp);
        fwrite(zero,1,_WIDTH,db->fp);
    }
}

void db_close(db* db)
{
    fclose(db->fp);
}

/*** function for testing ***/
char *random_str() {
    int i;
    char *alphabet = "abcdefghijklmnopqrstuvwxyz";
    char *string = malloc(33);
    for (i=0; i<32; i++) {
        string[i] = alphabet[rand()%26];
    }
    string[i] = 0;
    return string;
}
/***************************/

void print_usage(void)
{
    printf("Please give database name:\n");
    printf("Usage:\n");
    printf("        ./tree [DATABASE NAME]\n");
    printf("Example:\n");
    printf("        ./tree ./test.db\n");
}

unsigned char *read_data(FILE *fp)
{
    size_t length;
    unsigned char *data;

    if(!fp) return 0;

    // get file length
    fseek(fp, 0, SEEK_END);
    length = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // read program source
    data = (char *)malloc(length);
    fread(data, sizeof(char), length, fp);
    data_length = length;

    return data;
}

cl_program load_program(cl_context context, cl_device_id device, const char* filename)
{
    FILE *fp = fopen(filename, "rt");
    size_t length;
    char *data;
    char *build_log;
    size_t ret_val_size;
    cl_program program = 0;
    cl_int status = 0;

    if(!fp) return 0;

    // get file length
    fseek(fp, 0, SEEK_END);
    length = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // read program source
    data = (char *)malloc(length + 1);
    fread(data, sizeof(char), length, fp);
    data[length] = '\0';

    // create and build program 
    program = clCreateProgramWithSource(context, 1, (const char **)&data, 0, 0);
    if (program == 0) return 0;

    status = clBuildProgram(program, 0, 0, 0, 0, 0);
    if (status != CL_SUCCESS) {
        printf("Error:  Building Program from file %s\n", filename);
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &ret_val_size);
        build_log = (char *)malloc(ret_val_size + 1);
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, ret_val_size, build_log, NULL);
        build_log[ret_val_size] = '\0';
        printf("Building Log:\n%s", build_log);
        return 0;
    }

    return program;
}

void copy_back(db *db, unsigned char **path, uint64_t *node_addrs)
{
    int i, j;

    //printf("copy back\n");
    return ;

    for (i = 0; i < _DEPTH; i++) {
        if (path[i] != NULL) {
            for (j = 0; j < _WIDTH; j++)
                db->path[i][j] = path[i][j];
        }
        db->node_addrs[i] = node_addrs[i];
    }
}

uint64_t db_search_in_mem(db *db, unsigned char* key, int* r_index)
{
    if( _HASH > strlen(key) ){
        unsigned char* okey = key;
        key = malloc(_HASH+1);
        memset(key,0x61,_HASH+1);
        strncpy(key,okey,strlen(okey));
    }
    uint64_t r_addr, addr, cindex;
    int i = SIZEOF_LONG+1, j = 0;
    unsigned char isleaf, check;
    int index = 0;
    unsigned char *path[_DEPTH] = {0};
    uint64_t node_addrs[_DEPTH] = {0};

    r_addr = *(long *)(data + 0);
    path[index] = (data + r_addr);
    node_addrs[index] = r_addr;
search:
    isleaf = path[index][0];
    for( ; i<_WIDTH; i+=(_HASH+SIZEOF_LONG) ){
        if( !strncmp(path[index]+i,key,_HASH) ){
            if( isleaf ){
                *r_index = index;
                i -= SIZEOF_LONG;
                cindex = from_big(path[index]+i);
                check = data[cindex];
//                printf("check=%d\n", check); fflush(stdout);
                copy_back(db, path, node_addrs);
                if( check == 0 ){
                    return 1;
                }
                return 0;
            }
            if( index >= _DEPTH ){
                *r_index = 0;
                return -1;
            }
            i += _HASH;
            addr = from_big(path[index]+i);
            ++index;
            path[index] = (data + addr);
//            printf("path=%x %x %x %x\n", path[index][0], path[index][1], path[index][2], path[index][3]); fflush(stdout);
            node_addrs[index] = addr;
            i = SIZEOF_LONG+1;
            goto search;
        }
        if( strncmp(path[index]+i,key,_HASH) > 0 ||
                path[index][i] == 0 ){
            if( isleaf ){
                *r_index = index;
                copy_back(db, path, node_addrs);
                return 1;
            }
            if( index >= _DEPTH ){
                *r_index = 0;
                return -1;
            }
            i -= SIZEOF_LONG;
            addr = from_big(path[index]+i);
            ++index;
            path[index] = (data + addr);
//            printf("path=%x %x %x %x\n", path[index][0], path[index][1], path[index][2], path[index][3]); fflush(stdout);
            node_addrs[index] = addr;
            i = SIZEOF_LONG+1;
            goto search;
        }
    }
}

double time_spent = 0.0;
uint64_t db_search_in_opencl(db *db, unsigned char *key, int *r_index)
{
    cl_event event;
    int i, j;
    size_t work_size;
    int r_value[DATA_SIZE] = {0};
    unsigned char path[_WIDTH * _DEPTH * DATA_SIZE];
    uint64_t node_addrs[_DEPTH * DATA_SIZE];
    clock_t begin, end;

    for (i = 0; i < _DEPTH * DATA_SIZE; i++) {
        node_addrs[i] = 0;
    }
    for (i = 0; i < _WIDTH * _DEPTH * DATA_SIZE; i++) {
        path[i] = 0;
    }

    cl_a = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(cl_char) * _HASH * DATA_SIZE, key, NULL);
    cl_b = clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(cl_int) * DATA_SIZE, r_index, NULL);
    cl_c = clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(cl_int) * DATA_SIZE, r_value, NULL);
    cl_d = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(cl_char) * data_length, data, NULL);
    cl_e = clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(cl_char) * _WIDTH * _DEPTH * DATA_SIZE, db->path, NULL);
    cl_f = clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(cl_ulong) * _DEPTH * DATA_SIZE, db->node_addrs, NULL);
    if (cl_a == 0 || 
        cl_b == 0 || 
        cl_c == 0 ||
        cl_d == 0 ||
        cl_e == 0 ||
        cl_f == 0) {
        printf("Can't create OpenCL buffer\n");
        goto release;
    }
    checkErr(clEnqueueWriteBuffer(queue, cl_a, CL_TRUE, 0, sizeof(cl_char) * _HASH * DATA_SIZE, key, 0, 0, 0),
            printf("ERROR ! WRITE BUFFER\n");
            goto release;);
    checkErr(clEnqueueWriteBuffer(queue, cl_b, CL_TRUE, 0, sizeof(cl_int) * DATA_SIZE, r_index, 0, 0, 0),
            printf("ERROR ! WRITE BUFFER\n");
            goto release;);
    checkErr(clEnqueueWriteBuffer(queue, cl_d, CL_TRUE, 0, sizeof(cl_char) * data_length, data, 0, 0, 0),
            printf("ERROR ! WRITE BUFFER\n");
            goto release;);
    checkErr(clEnqueueWriteBuffer(queue, cl_e, CL_TRUE, 0, sizeof(cl_char) * _WIDTH * _DEPTH * DATA_SIZE, path, 0, 0, 0),
            printf("ERROR ! WRITE BUFFER\n");
            goto release;);
    checkErr(clEnqueueWriteBuffer(queue, cl_f, CL_TRUE, 0, sizeof(cl_ulong) * _DEPTH * DATA_SIZE, node_addrs, 0, 0, &event),
            printf("ERROR ! WRITE BUFFER\n");
            goto release;);
    clWaitForEvents(1, &event);

    adder = clCreateKernel(program, "adder", &err);
    if (err == CL_INVALID_KERNEL_NAME) printf("CL_INVALID_KERNEL_NAME\n");
    checkExit(adder, "Can't load kernel\n");

    clSetKernelArg(adder, 0, sizeof(cl_mem), &cl_a);
    clSetKernelArg(adder, 1, sizeof(cl_mem), &cl_b);
    clSetKernelArg(adder, 2, sizeof(cl_mem), &cl_c);
    clSetKernelArg(adder, 3, sizeof(cl_mem), &cl_d);
    clSetKernelArg(adder, 4, sizeof(cl_mem), &cl_e);
    clSetKernelArg(adder, 5, sizeof(cl_mem), &cl_f);

    work_size = DATA_SIZE;

    begin = clock();
    checkErr(clEnqueueNDRangeKernel(queue, adder, 1, 0, &work_size, 0, 0, 0, &event),
             printf("Can't enqueue kernel\n");
            );
    clWaitForEvents(1, &event);
    end = clock();

    checkErr(clEnqueueReadBuffer(queue, cl_b, CL_TRUE, 0, sizeof(cl_int) * DATA_SIZE, r_index, 0, 0, 0),
             printf("Can't enqueue read buffer\n");
            );
    checkErr(clEnqueueReadBuffer(queue, cl_c, CL_TRUE, 0, sizeof(cl_int) * DATA_SIZE, r_value, 0, 0, 0),
             printf("Can't enqueue read buffer\n");
            );
    checkErr(clEnqueueReadBuffer(queue, cl_e, CL_TRUE, 0, sizeof(cl_char) * _WIDTH * _DEPTH * DATA_SIZE, path, 0, 0, 0),
             printf("Can't enqueue read buffer\n");
            );
    checkErr(clEnqueueReadBuffer(queue, cl_f, CL_TRUE, 0, sizeof(cl_ulong) * _DEPTH * DATA_SIZE, node_addrs, 0, 0, 0),
             printf("Can't enqueue read buffer\n");
            );

    for (i = 0; i < DATA_SIZE; i++) {
//        printf("%d, %d\n", r_value[i], r_index[i]);
    }
//    printf("\n");
    time_spent += (double)(end - begin) / CLOCKS_PER_SEC;
release:
    clReleaseKernel(adder);
    clReleaseMemObject(cl_a);
    clReleaseMemObject(cl_b);
    clReleaseMemObject(cl_c);
    clReleaseMemObject(cl_d);
    clReleaseMemObject(cl_e);
    clReleaseMemObject(cl_f);

    return 0;
}

void read_test_input(db *my_db)
{
    char key[DATA_SIZE * _HASH] = {0}, value[1024] = {0};
    int cnt = 0, r_index[DATA_SIZE] = {0}, i;
    char *v = NULL;
    char c;

    while (scanf("%[^\n]\n", &key[(cnt % 10) * _HASH]) != EOF) {
        cnt++;
        if (cnt % DATA_SIZE == 0) {
            for (i = 0; i < DATA_SIZE; i++)
                r_index[i] = 0;
            db_search_in_opencl(my_db, key, r_index);
        }
        /*
        sprintf(value, "%d", cnt);
        v = db_get(my_db, key);
        if (v!= NULL && strcmp(value, v) != 0) {
            printf("(%s, %s) while id is %s\n", key, v, value);
        }
      */  
    }   
    printf("elapsed: %lf\n", time_spent);
}

int main(int argc, char **argv)
{
    int num_total_devices = 0;
    char devname[16][256] = {{0}};
    size_t cb;
    cl_float a[DATA_SIZE], b[DATA_SIZE], res[DATA_SIZE];
    int i;
    db my_db;

    for(i = 0; i < DATA_SIZE; i++) {
        a[i] = 1;//(rand() % 100) / 100.0;
        b[i] = 1;//(rand() % 100) / 100.0;
        res[i] = 0;
    }

    if (argc != 2) {
        print_usage();
        exit(1);
    }

    db_init(&my_db, argv[1]);
    data = read_data(my_db.fp);
    db_put(&my_db, "hello", "world");
    char* value = db_get(&my_db, "hello");
    printf("%s\n", value);
    db_close(&my_db);

    checkErr(clGetPlatformIDs(0, 0, &num), 
             printf("Unable to get platforms\n");
             return 0;
             );

    platforms = (cl_platform_id *)malloc(sizeof(cl_platform_id) * num);
    checkErr(clGetPlatformIDs(num, platforms, NULL), 
             printf("Unable to get platform ID\n");
             return 0;
             );

    checkErr(clGetPlatformIDs(0, 0, &num), 
             printf("Unable to get platforms\n");
             return 0;
             );

    prop[0] = CL_CONTEXT_PLATFORM;
    prop[1] = (cl_context_properties)platforms[0];
    prop[2] = 0;
    context = clCreateContextFromType(prop, CL_DEVICE_TYPE_ALL, NULL, NULL, NULL);
    checkExit(context, "Can't create OpenCL context\n");

    clGetContextInfo(context, CL_CONTEXT_DEVICES, 0, NULL, &cb);
    devices = (cl_device_id *)malloc(cb);
    clGetContextInfo(context, CL_CONTEXT_DEVICES, cb, devices, 0);
    checkExit(cb, "Can't get devices\n");
    num_total_devices = cb / sizeof(cl_device_id);

    for (i = 0; i < num_total_devices; i++) {
        clGetDeviceInfo(devices[i], CL_DEVICE_NAME, 0, NULL, &cb);
        clGetDeviceInfo(devices[i], CL_DEVICE_NAME, cb, devname, 0);
        printf("Device(%d/%d): %s\n", i, num_total_devices, devname[i]);
    }

    queue = clCreateCommandQueue(context, devices[0], 0, 0);
    checkExit(queue, "Can't create command queue\n");

    program = load_program(context, devices[0], "shader.cl");
    checkExit(program, "Fail to build program\n");

    read_test_input(&my_db);

release:
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);

    free(data);
    return 0; 
}

