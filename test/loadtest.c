#include <windows.h>
#include <stdio.h>
int main(int argc, char **argv) {
    HMODULE m = LoadLibraryA(argc > 1 ? argv[1] : "autopot.dll");
    printf("LoadLibrary -> %p\n", (void *)m);
    Sleep(500);
    return m ? 0 : 1;
}
