#define _HASH 128
#define _ORDER 99
#define _WIDTH (1+_HASH*_ORDER+SIZEOF_LONG*(_ORDER+1))
#define _DEPTH 10
#define _MAX 0xf4240
#define SIZEOF_LONG sizeof(ulong)

ulong from_big(__global unsigned char* buf) {
    ulong val = 0;
    int i;
    for( i=0; i<sizeof(ulong); ++i )
        val |= (ulong) buf[i] << (56-(i*8));
    return val;
}

ulong from_little(__global unsigned char* buf) {
    ulong val = 0;
    int i;
    for( i=0; i<sizeof(ulong); ++i )
        val |= (ulong) buf[i] << (i*8);
    return val;
}

int strncmp(__global unsigned char * str1, __global unsigned char * str2, int num)
{
    int i;

    for (i = 0; i < num && str1[i] != 0 && str2[i] != 0; i++) {
        if (str1[i] > str2[i]) return 1;
        if (str1[i] < str2[i]) return -1;
    }

    if (str1[i] == 0 && str2[i] != 0) return -1;
    if (str1[i] != 0 && str2[i] == 0) return 1;
    return 0;
}

void copy(__global unsigned char *a, __global unsigned char *b, int size)
{
    int i;

    for (i = 0; i < size; i++)
        a[i] = b[i];
}

ulong db_search_in_mem(__global unsigned char* key, __global int* r_index, __global unsigned char *data, __global unsigned char *path, __global ulong *node_addrs)
{
    ulong r_addr, addr, cindex;
    int i = SIZEOF_LONG+1, j = 0;
    unsigned char isleaf, check;
    int index = 0;

    r_addr = from_little(data);
    copy(path + index * _WIDTH, data + r_addr, _WIDTH);
    node_addrs[index] = r_addr;
search:
    isleaf = path[index * _WIDTH];
    for( ; i<_WIDTH; i+=(_HASH+SIZEOF_LONG) ){
        if (!strncmp(&path[index * _WIDTH + i], key, _HASH) ){
            if( isleaf ){
                *r_index = index;
                i -= SIZEOF_LONG;
                cindex = from_big(&path[index * _WIDTH + i]);
                check = data[cindex];
//                printf("check=%d\n", check); fflush(stdout);
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
            addr = from_big(&path[index * _WIDTH + i]);
            ++index;
            copy(path + index * _WIDTH, data + addr, _WIDTH);
//            printf("path=%x %x %x %x\n", path[index][0], path[index][1], path[index][2], path[index][3]); fflush(stdout);
            node_addrs[index] = addr;
            i = SIZEOF_LONG+1;
            goto search;
        }
        if (strncmp(&path[index * _WIDTH + i], key, _HASH) > 0 ||
                path[index * _WIDTH + i] == 0 ){
            if( isleaf ){
                *r_index = index;
                return 1;
            }
            if( index >= _DEPTH ){
                *r_index = 0;
                return -1;
            }
            i -= SIZEOF_LONG;
            addr = from_big(&path[index * _WIDTH + i]);
            ++index;
            copy(path + index * _WIDTH, data + addr, _WIDTH);
//            printf("path=%x %x %x %x\n", path[index][0], path[index][1], path[index][2], path[index][3]); fflush(stdout);
            node_addrs[index] = addr;
            i = SIZEOF_LONG+1;
            goto search;
        }
    }
}

__kernel void adder(
__global unsigned char *key, 
__global int *r_index, 
__global int *r_value, 
__global unsigned char *data, 
__global unsigned char *path, 
__global ulong *node_addrs)
{
	int idx = get_global_id(0);
    
    r_value[idx] = db_search_in_mem(
                                    &key[idx * _HASH], 
                                    &r_index[idx], 
                                    data, 
                                    &path[idx * _WIDTH * _DEPTH], 
                                    &node_addrs[idx * _DEPTH]);
}
