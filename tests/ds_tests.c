#define DS_IMPLEMENTATION
#include "../ds.h"

int main(int argc, char *argv[]) {
    Arena arena = arena_create(1024*1024, 8);
    arena_destroy(&arena);
}