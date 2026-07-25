/* test_hello.c — Simple test program for etherload/etherdbg -r testing
 *
 * Prints "HELLO FROM ETHERDBG!" and changes the border colour.
 * Compile with cc45 or cc65 for C64 mode ($0801).
 *
 * For quick testing without a compiler, use the pre-built PRG below.
 */

#include <stdio.h>

int main(void)
{
    /* Change border to cyan */
    *(unsigned char*)0xD020 = 3;
    /* Change background to black */
    *(unsigned char*)0xD021 = 0;

    printf("\n\n");
    printf("  ****************************\n");
    printf("  *                          *\n");
    printf("  *  HELLO FROM ETHERDBG!    *\n");
    printf("  *                          *\n");
    printf("  *  IF YOU SEE THIS, THE    *\n");
    printf("  *  TRANSFER WORKED!        *\n");
    printf("  *                          *\n");
    printf("  ****************************\n");
    printf("\n");

    return 0;
}
