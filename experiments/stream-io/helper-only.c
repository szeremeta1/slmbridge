#define _GNU_SOURCE
#define RS_SELFTEST
#include "bridge.c"
int main(int argc, char **argv)
{
    if (argc != 3) return 99;
    return helper_main(atoi(argv[1]), atoi(argv[2]));
}
