void test(__global float *a, __global float *b, __global float *res)
{
    int i, j;

    for (i = 0; i < 1000; i++)
            *res += *a + *b;
}

__kernel void adder(__global const float* a, __global const float* b, __global float* result)
{
	int idx = get_global_id(0);
    result[idx] = 0;
    test(&a[idx], &b[idx], &result[idx]);
}

